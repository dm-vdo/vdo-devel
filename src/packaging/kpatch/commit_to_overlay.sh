#!/bin/bash
# commit_to_overlay.sh
# Create a kernel tree overlay branch from a commit or series of commits.
#
# This script is intended to be run from a developer fork of dm-vdo/vdo-devel
# after the relevant PR(s) have been merged.
#
# In addition to the selected commits, the script will overlay additional bogus
# "difference-capture" commits to account for discrepancies between the source
# and target trees. The script will attempt to remove these commits via rebase
# at the end of the process, but that rebase may cause conflicts and need to be
# resolved manually in some cases. The extra commits are clearly labelled as
# "Bogus commit" so it should be easy to determine what to drop.
#
# Usage:
# ./commit_to_overlay.sh <kernel_tree_specifier> <kernel_tree_branch_name>
#                        <overlay_branch_name> [ -c | -m ] args...
#
# Input Type Options:
#  -c  Process a commit or series of commits into a kernel overlay branch
#  -m  Process all changes merged in the named merge commit
#
# Accepted Arguments:
#  Commit SHA(s) ordered from oldest to newest
#
# ** Multiple arguments must be delimited with a space **
#
# Notes:
# - The <kernel_tree_specifier> argument can be either a URL or a path.
#
#   - If the specifier is a URL, the working kernel repository will be cloned
#     from that URL.  This local clone will be removed upon exiting the script,
#     unless the DEBUG flag is set or the manual push option is selected. It is
#     recommended that the SSH URL is used, as GitHub prompts for a username
#     and password when an HTTPS URL is used.  The <kernel_tree_branch_name>
#     argument must be a branch in the given repository, which will be the
#     starting point of the new overlay branch.
#
#   - If the specifier is a path, it must point to the top level of a kernel
#     clone, and must not be a bare repository. This clone will create the new
#     overlay branch but it will not otherwise be altered. A relative path will
#     be interpreted relative to the current working directory.  The
#     <kernel_tree_branch_name> argument must resolve to a commit in the given
#     repository, which will be the starting point of the new overlay
#     branch. Additionally, if the <kernel_tree_branch_name> contains a slash,
#     the substring before the first slash is used as the remote to push the
#     new branch to. The default remote is 'origin'.
#
# - The single commits option (-c) relies on commits appearing in commit
#   order. If commits are provided in a different order than in the source
#   repo, strange things may happen.
#
# - The local vdo source tree will be cloned into a local absolute path. By
#   default, it will be removed upon exiting the script, unless the DEBUG flag
#   is set.
#
# - Output from building the VDO tree while processing each change can be found
#   in the /tmp/overlay_build_log* file. The log is removed at the end of each
#   loop iteration upon success.  It is retained until the commit_to_overlay.sh
#   script is re-run upon failure, or when the DEBUG flag is set.
#
# Potential Future Modifications:
#
# - Add a proper optional argument for specifying the remote instead of using
#   part of <kernel_tree_branch_name>.
##

if [[ -z ${DEBUG} ]]; then
  DEBUG=1
fi

TOOL=$(basename $0)
RUN_DIR=$(pwd)
SOURCE_REPO_URL=$(git config --get remote.origin.url)
MANUAL_PUSH=1
COLOR_RED='\033[0;31m'
NO_COLOR='\033[0m'

# Array of command line arguments
ADDITIONAL_ARGS=()
# Array of commit SHAs to be added to the overlay
COMMIT_SHAS=()
# Array of commit SHAs to turn into bogus commits
FAKE_SHAS=()
# Array of bogus commit SHAs to be removed at the end
REMOVE_SHAS=()
# Array of strings to be used in the configuration message output to stdout
CONFIG_MSG=()
# Indicated input type
INPUT_TYPE=
# Name to give the kernel overlay branch being created
OVERLAY_BRANCH=
# Path to the kernel tree
LINUX_SRC=
# Path to the cloned kernel tree, if any
CLONED_KERNEL_TREE=
# URL or path for the kernel repo where the overlay branch will be created
KERNEL_REPO_URL=
# Kernel tree branch to base the overlay on
KERNEL_BRANCH=
# Kernel remote to push changes to
KERNEL_REMOTE=origin
# Function to call to process the input(s)
PROCESS_INPUT_FX=
# Merge commit to find commits to apply
MERGE_COMMIT=

# Expand the arguments provided into the necessary parameters to operate on.
process_args() {
  test "$#" -lt 5 && print_usage
  if [ -z "$1" ] || [ -z "$2" ] || [ -z "$3" ] || [ -z "$4" ] || [ -z "$5" ]; then
    print_usage
  fi

  KERNEL_REPO_URL=$1
  KERNEL_BRANCH=$2
  OVERLAY_BRANCH=$3
  INPUT_TYPE=$4
  shift 4
  ADDITIONAL_ARGS=("$@")

  # Remove any artifacts from previous runs
  rm -f /tmp/overlay_build_log*

  # Set parameters based on input type
  case "${INPUT_TYPE}" in
    "-c"|"--c"|"c")
      PROCESS_INPUT_FX=process_commits
      CONFIG_MSG=("commit" "Commit(s) to be used in overlay:")
      ;;
    "-m"|"--m"|"m")
      PROCESS_INPUT_FX=process_merge
      CONFIG_MSG=("merge" "Merge commit to be used in overlay:")
      MERGE_COMMIT=${ADDITIONAL_ARGS[0]}
      ;;
    *)
      echo -e "${COLOR_RED}ERROR: Invalid input type specified${NO_COLOR}"
      print_usage
      ;;
  esac

  if [[ ${KERNEL_REPO_URL} =~ : ]]; then
    echo "Using kernel repo URL (${KERNEL_REPO_URL})"
    git ls-remote --exit-code ${KERNEL_REPO_URL} ${KERNEL_BRANCH} &>/dev/null
    return=$?
    if [ $return == 128 ]; then
      echo -e "${COLOR_RED}ERROR: Input URL (${KERNEL_REPO_URL}) is not" \
              "a valid repository${NO_COLOR}"
      print_usage
    elif [ $return == 2 ]; then
      echo -e "${COLOR_RED}ERROR: Branch ${KERNEL_BRANCH} not found on repo" \
              "$(get_repo_name ${KERNEL_REPO_URL})${NO_COLOR}"
      print_usage
    fi
  else
    echo "Using kernel repo path (${KERNEL_REPO_URL})"
    if [[ ${KERNEL_REPO_URL} =~ ^/ ]]; then
      LINUX_SRC=${KERNEL_REPO_URL}
    else
      LINUX_SRC=${RUN_DIR}/${KERNEL_REPO_URL}
    fi
    if [[ ${KERNEL_BRANCH} =~ ^([^/]*)/ ]]; then
      git -C ${LINUX_SRC} remote get-url ${BASH_REMATCH[1]} &>/dev/null
      if [ $? == 0 ]; then
        KERNEL_REMOTE=${BASH_REMATCH[1]}
      else
        echo -e "${COLOR_RED}WARNING: ${BASH_REMATCH[1]} is not a valid remote"
        echo -e "Using 'origin' instead${NO_COLOR}"
      fi
    fi
    KERNEL_REPO_URL=$(git -C ${LINUX_SRC} remote get-url ${KERNEL_REMOTE})

    # Verify that the provided repository is usable.
    IS_BARE=$(git -C ${LINUX_SRC} rev-parse --is-bare-repository)
    if [[ ${IS_BARE} =~ true ]]; then
      echo -e "${COLOR_RED}ERROR: ${LINUX_SRC} is a bare repository${NO_COLOR}"
      print_usage
    fi
    git -C ${LINUX_SRC} rev-parse --verify --quiet ${KERNEL_BRANCH}
    if [ $? != 0 ]; then
      echo -e "${COLOR_RED}ERROR: ${KERNEL_BRANCH} does not exist in local" \
              "tree ${LINUX_SRC}${NO_COLOR}"
      print_usage
    fi
  fi

  # Verify the overlay branch name does not already exist
  git ls-remote --exit-code ${KERNEL_REPO_URL} ${OVERLAY_BRANCH} &>/dev/null
  if [ $? == 0 ]; then
    echo -e "${COLOR_RED}ERROR: Branch ${OVERLAY_BRANCH} already exists on repo" \
            "$(get_repo_name ${KERNEL_REPO_URL})${NO_COLOR}"
    print_usage
  fi
}

# Print the script usage information to stdout
print_usage() {
  indent=$((${#TOOL}+2))

  echo
  echo "${TOOL}: Create a kernel tree overlay branch from a commit or a"
  printf "%${indent}s%s\n" ' ' 'series of commits.'
  echo
  echo "  Usage: "
  echo "  ./${TOOL} <kernel_tree_specifier> <kernel_tree_branch_name>"
  printf "%$((${indent}+3))s%s\n" ' ' '<overlay_branch_name> [ -c | -m ] args...'
  echo
  echo "  Input Type Options:"
  echo "     -c  Process a commit or series of commits into a kernel overlay branch"
  echo "     -m  Process all changes merged in the named merge commit"
  echo
  echo "  Accepted Arguments:"
  echo "     Commit SHA(s) ordered from oldest to newest"
  echo
  echo "  ** Multiple arguments must be delimited with a space **"
  echo
  echo "  <kernel_tree_specifier> can be either a URL or a path:"
  echo "  * If a URL is given, the kernel repository will be cloned from that URL"
  echo "  * If a path is given, it must be the top level of a non-bare kernel clone"
  echo "  * A relative path will be resolved from the current working directory"
  echo
  echo "  <kernel_tree_branch_name> argument must be a branch or commit, which will"
  echo "  be the starting point of the new overlay branch"
  echo
  echo "  If <kernel_tree_specifier> is a path and <kernel_tree_branch_name> contains"
  echo "  a slash, the portion of <kernel_tree_branch_name> before the first slash"
  echo "  will be used as the name of the remote to push to. In all other cases, the"
  echo "  remote defaults to 'origin'."
  echo
  echo "  If the script exits with an error, details can be found in the build logs"
  echo "  in /tmp/overlay_build_log*"
  exit
}

# Extract the repo name from the URL
get_repo_name() {
  if [[ $1 =~ 'https://' ]]; then
    echo "$1" | awk -F '^.*\.com/|\.git' '{print $2}' 2>/dev/null
  else
    echo "$1" | awk -F '^.*:|\.git' '{print $2}' 2>/dev/null
  fi
}

# Print the input configuration to stdout
print_config() {
  title_str="Proceeding with the following configuration:"

  echo
  echo "$title_str"
  printf '=%.0s' $(seq 1 ${#title_str})
  echo # Intentional blank line
  echo "* Source repo: ${SOURCE_REPO_URL}"
  echo "* Kernel repo: ${KERNEL_REPO_URL}"
  echo "* Overlay branch to be created on kernel repo: ${OVERLAY_BRANCH}"
  echo "* Processing based on: ${CONFIG_MSG[0]}"
  echo "* ${CONFIG_MSG[1]}"

  for arg in ${ADDITIONAL_ARGS[@]}; do
    printf '%4s%s\n' '' "${arg}"
  done

  printf '=%.0s' $(seq 1 ${#title_str})
  echo
  echo
}

# Process the input commit SHA(s), verifying they are valid
process_commits() {
  echo
  echo "Validating input commit SHAs..."
  for commit in ${ADDITIONAL_ARGS[@]}; do
    git show ${commit} &>/dev/null
    if [ $? != 0 ]; then
      echo -e "${COLOR_RED}ERROR: Invalid commit SHA encountered ($commit)"
      echo -e "Please verify the input arguments and re-run this script${NO_COLOR}"
      exit
    fi

    # Compare the first 7 characters to find duplicates, to account for partial SHAs
    # Only the first valid entry will be kept
    if [[ ! ${COMMIT_SHAS[@]} =~ ${commit::7} ]]; then
      parent=$(git rev-parse ${commit}^1)
      if [[ ${COMMIT_SHAS[@]} =~ ${parent::7} ]]; then
        # If this commit's parent is already in the list, we don't need a bogus commit
        COMMIT_SHAS+=(${commit})
      else
        # Apply this commit's parent as a bogus commit, but remember it for later removal
        COMMIT_SHAS+=(${parent} ${commit})
        FAKE_SHAS+=(${parent})
      fi
    else
      echo "Duplicate commit SHA found and removed ($commit)"
    fi
  done

  prompt_user_verify
}

# Process the input commit SHA(s), verifying they are valid
process_merge() {
  echo
  echo "Validating input commit SHA ($MERGE_COMMIT)..."
  git show ${MERGE_COMMIT} &>/dev/null
  if [ $? != 0 ]; then
    echo -e "${COLOR_RED}ERROR: Invalid commit SHA ($MERGE_COMMIT)"
    echo -e "Please verify the input arguments and re-run this script${NO_COLOR}"
    exit
  fi

  git show ${MERGE_COMMIT}^2 &>/dev/null
  if [ $? != 0 ]; then
    echo -e "${COLOR_RED}ERROR: Commit SHA ($MERGE_COMMIT) is not a merge"
    echo -e "Please verify the input arguments and re-run this script${NO_COLOR}"
    exit
  fi

  # Apply this merge commit's parent as a bogus commit, but remember it for later removal
  parent=$(git rev-parse ${MERGE_COMMIT}^1)
  COMMIT_SHAS=(${parent})
  FAKE_SHAS+=(${parent})

  # List all commits merged in this commit
  COMMIT_SHAS+=($(git rev-list --reverse --no-merges ${MERGE_COMMIT}^2 ^${MERGE_COMMIT}^1))
  if [[ ${#COMMIT_SHAS[@]} == 1 ]]; then
    echo "No commits identified"
    exit
  fi

  prompt_user_verify
}

# Prompt the user to verify the commits that will be included in the overlay
prompt_user_verify() {
  user_input=""

  while [[ ! ${user_input} =~ ^(y|Y) ]]; do
    echo "Commits to be included in the kernel overlay:"
    for sha in ${COMMIT_SHAS[@]}; do
      if [[ ! ${FAKE_SHAS[@]} =~ ${sha} ]]; then
        printf '%2s%s\n' '' "$(git show --no-patch --oneline ${sha})"
      fi
    done

    echo
    read -p "Is this correct? [y - proceed | n - exit]: " user_input
    case "${user_input}" in
      "y"*|"Y"*)
        echo "Proceeding."
        ;;
      "n"*|"N"*)
        echo "Exiting."
        exit
        ;;
      *)
        echo -e "${COLOR_RED}ERROR: Invalid response."
        echo -e "Please enter 'y' to proceed, or 'n' to exit.${NO_COLOR}"
        echo
        ;;
    esac
  done
}

# Set up the working repositories, cloning them if necessary
setup_repos() {
  echo
  echo "Cloning VDO tree $(get_repo_name ${SOURCE_REPO_URL})"
  VDO_TREE=${RUN_DIR}/$(mktemp -d vdo-raw-XXXXXX)
  git clone $SOURCE_REPO_URL ${VDO_TREE}

  if [[ -z ${LINUX_SRC} ]]; then
    LINUX_SRC=${RUN_DIR}/$(mktemp -d vdo-kernel-XXXXXX)
    CLONED_KERNEL_TREE=${LINUX_SRC}

    # Shallow clone the specified dm-linux repo branch into LINUX_SRC
    echo "Cloning kernel repo $(get_repo_name ${KERNEL_REPO_URL})/${KERNEL_BRANCH}"
    git clone --branch ${KERNEL_BRANCH} --depth 1 ${KERNEL_REPO_URL} ${LINUX_SRC}
  fi

  LINUX_MD_SRC=${LINUX_SRC}/drivers/md
  LINUX_VDO_SRC=${LINUX_MD_SRC}/dm-vdo
  LINUX_DOC_SRC=${LINUX_SRC}/Documentation/admin-guide/device-mapper

  cd $LINUX_SRC
  git checkout -b ${OVERLAY_BRANCH} ${KERNEL_BRANCH}
}

# Build and overlay one commit on LINUX_SRC
overlay_commit() {
  change=$1
  echo
  echo "Processing changes from commit $change"

  cd ${VDO_TREE}

  # Clean up from the last build, if necessary
  if [[ "${change}" != "${COMMIT_SHAS[0]}" ]]; then
    echo -n "Cleaning up from the last build... "
    git clean -fdxq
    echo "Done"
  fi

  # This puts us in a detached HEAD state
  # Perhaps we should suppress the warning
  #sudo git config --system advice.detachedHead "false"
  echo "Checking out the change to build and post it"
  git checkout ${change}

  # Capture the commit message from the change
  echo "Creating the initial commit file using the commit message from the change"
  commit_file=$(mktemp /tmp/overlay_commit_file.XXXXX)
  if [[ ! ${FAKE_SHAS[@]} =~ ${change} ]]; then
    git log --format=%B -n 1 ${change} > ${commit_file}
  else
    echo "Bogus commit: Drop before submitting upstream" > ${commit_file}
    echo "" >> ${commit_file}
    echo "This commit is intended to capture tree differences between" >> ${commit_file}
    echo "vdo-devel and upstream so they don't get combined with desired" >> ${commit_file}
    echo "changes. This commit must be dropped from the series before" >> ${commit_file}
    echo "making a PR or submitting to upstream." >> ${commit_file}
  fi

  # Capture authorship information for use later
  commit_author_email=$(git log --format=%aE -1 ${change})
  commit_author_name=$(git log --format=%aN -1 ${change})
  commit_date=$(git log --format=%ad -1 ${change})
  gitauthor="--author=\"${commit_author_name} <${commit_author_email}>\""
  gitdate="--date=\"${commit_date}\""

  # Build the perl tree so the prepare script can run.
  build_log_file=$(mktemp /tmp/overlay_build_log.XXXXX)
  echo -n "Building the VDO perl directory... "
  make -C src/perl >> ${build_log_file} 2>&1
  if [[ $? != 0 ]]; then
    echo
    echo -e "${COLOR_RED}ERROR: Building the VDO tree failed."
    echo -e "See ${build_log_file} for more information.${NO_COLOR}"
    exit
  fi
  echo "Done"

  # Generate the kernel overlay and apply it to the linux tree
  echo "Generating the kernel overlay"
  cd src/packaging/kpatch

  export LINUX_SRC=${LINUX_SRC}
  make prepare >> ${build_log_file}
  if [[ $? != 0 ]]; then
    echo -e "${COLOR_RED}ERROR: Preparing kernel files failed."
    echo -e "See ${build_log_file} for more information.${NO_COLOR}"
    exit
  fi

  # Copy the prepared kernel products into the target repo
  WORK_DIR=work/kvdo-*
  rm -rf ${LINUX_VDO_SRC} && \
  mkdir -p ${LINUX_VDO_SRC} && \
  cp -r ${WORK_DIR}/dm-vdo/* ${LINUX_VDO_SRC} && \
  sed -i -E -e 's/(#define	CURRENT_VERSION).*/\1 "8.3.0.65"/' \
    ${LINUX_VDO_SRC}/dm-vdo-target.c && \
  cp -f ${WORK_DIR}/vdo.rst ${WORK_DIR}/vdo-design.rst ${LINUX_DOC_SRC}/
  if [[ $? != 0 ]]; then
    echo -e "${COLOR_RED}ERROR: Generating the kernel overlay failed.${NO_COLOR}"
    exit
  fi

  # Commit the changes to the target overlay branch, if any
  echo "Applying the kernel overlay to branch '${OVERLAY_BRANCH}'"
  git -C ${LINUX_SRC} diff --quiet HEAD
  if [[ $? == 0 ]]; then
    echo "Nothing to apply - moving on."
  else
    git -C ${LINUX_SRC} add . && \
    git -C ${LINUX_SRC} commit -s "${gitauthor}" "${gitdate}" --file "${commit_file}"
    if [[ $? == 0 ]]; then
      echo "Committed"
      if [[ ${FAKE_SHAS[@]} =~ ${change} ]]; then
        # Remember the new commit SHA for bogus commits. The removal list must be
        # ordered newest to oldest, so that rebasing doesn't rewrite the SHAs of
        # commits that haven't been handled yet.
        REMOVE_SHAS=($(git -C ${LINUX_SRC} rev-parse HEAD) "${REMOVE_SHAS[@]}")
      fi
    else
      echo -e "${COLOR_RED}ERROR: Applying the kernel overlay to branch" \
              "'${OVERLAY_BRANCH}' failed."
      exit
    fi
  fi

  # Clean up
  echo "Cleaning up build artifacts"

  rm -fv ${commit_file}

  if [[ ${DEBUG} != 0 ]]; then
    rm -fv ${build_log_file}
  fi
}
 
# Remove each commit in REMOVE_SHAS from the branch.
remove_bogus_commits () {
  # This list need to be ordered newest to oldest so rebases don't
  # rewrite the SHAs of commits we still need to remove.
  for remove_change in ${REMOVE_SHAS[@]}; do
    git -C ${LINUX_SRC} rebase --onto ${remove_change}^1 ${remove_change} ${OVERLAY_BRANCH}
    if [[ $? != 0 ]]; then
      git -C ${LINUX_SRC} rebase --abort
      echo -e "${COLOR_RED}ERROR: Removal of bogus difference-capture commits failed."
      echo -e "Fix branch manually by dropping these commits:"
      echo -e "  ${REMOVE_SHAS[@]}${NO_COLOR}"
      exit
    fi
  done
}
 
# Prompt the user to push the overlay branch upstream
prompt_user_push() {
  user_input=""

  while [[ ! ${user_input} =~ ^(y|Y|m|M) ]]; do
    echo
    echo "Please verify everything expected has been applied to overlay branch" \
         "${OVERLAY_BRANCH}."
    echo "Ready to push ${OVERLAY_BRANCH} to $(get_repo_name ${KERNEL_REPO_URL})?"
    echo -n "[y - yes, push | n - no, delete | m - keep branch for manual push later]: "
    read user_input

    case "${user_input}" in
      "y"*|"Y"*)
        echo "Proceeding with push."
        echo
        ;;
      "n"*|"N"*)
        exit
        ;;
      "m"*|"M"*)
        echo "Retaining ${LINUX_SRC} and branch ${OVERLAY_BRANCH} for manual push later."
        MANUAL_PUSH=0
        ;;
      *)
        echo -e "${COLOR_RED}ERROR: Invalid response."
        echo -e "Please enter 'y' to proceed with push, 'n' to delete the branch and exit,"
        echo -e "or 'm' to retain the branch for manual push at a later time.${NO_COLOR}"
        echo
        ;;
    esac
  done
}

# Cleanup artifacts
_cleanup() {
  cd $RUN_DIR
  echo
  echo "Cleaning up and exiting"

  if [[ -d ${LINUX_SRC} ]] && [[ -d ${VDO_TREE} ]] && [[ ${DEBUG} != 0 ]]; then
    if [[ ${MANUAL_PUSH} != 0 ]] && [[ -d ${CLONED_KERNEL_TREE} ]]; then
      echo "Removing ${CLONED_KERNEL_TREE}"
      rm -rf ${CLONED_KERNEL_TREE}
    fi

    echo "Removing ${VDO_TREE}"
    rm -rf ${VDO_TREE}
  fi

  if [[ -e ${commit_file} ]] && [[ -f ${commit_file} ]]; then
    rm -fv ${commit_file}
  fi

  unset -f DEBUG LINUX_SRC
}

###############################################################################
# main()
trap _cleanup EXIT

process_args $@
print_config
$PROCESS_INPUT_FX
setup_repos

# Loop over each commit in COMMIT_SHAS and overlay it on LINUX_SRC
for change in ${COMMIT_SHAS[@]}; do
  overlay_commit "${change}"
done
remove_bogus_commits

# Push the kernel overlay branch to the remote kernel repository
cd ${LINUX_SRC}
prompt_user_push

if [[ ${MANUAL_PUSH} != 0 ]]; then
  git push -u ${KERNEL_REMOTE} ${OVERLAY_BRANCH}
fi

exit 0
