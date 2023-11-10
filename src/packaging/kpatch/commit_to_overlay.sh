#!/bin/bash
# commit_to_overlay.sh
# Create a kernel tree overlay branch from a commit, series of commits, or local branch.
#
# This script is intended to be run from a developer fork of dm-vdo/vdo-devel after the relevant
# PR(s) have been merged.
#
# Usage:
# ./commit_to_overlay.sh <kernel_tree_repo_URL> <kernel_tree_branch_name>
#                        <overlay_branch_name> [ -c | -b | -m ] args...
#
# Input Type Options:
#  -c  Process a commit or series of commits into a kernel overlay branch
#  -b  Process all changes on a local branch into a kernel overlay branch
#
# Accepted Arguments:
#  Commit SHA(s) ordered from oldest to newest
#  Local branch name
#
# ** Multiple arguments must be delimited with a space **
#
# Notes:
# - It is recommended that the SSH kernel_tree_repo_URL is input, as GitHub prompts for a
#   username and password when an HTTPS URL is used.
# - The local branch option (-b) relies on detecting differences between the input branch and
#   origin/HEAD. It is only effective before a rebase of the fork is performed.
# - Both the remote kernel tree and local vdo source tree will be cloned into a local absolute path
#   during this process. By default, they will both be removed upon exiting the script, unless the
#   DEBUG flag is set.
# - Regardless of the DEBUG flag setting, the kernel tree clone will be retained if the user elects
#   (when prompted) to manually push the successfully created overlay branch at a later time.
# - Output from building the VDO tree while processing each change can be found in the
#   /tmp/overlay_build_log* file. The log is removed at the end of each loop iteration upon success.
#   It is retained until the commit_to_overlay.sh script is re-run upon failure, or when the DEBUG
#   flag is set.
#
# Potential Future Modifications:
# - Remove the kernel_tree_branch_name input argument if it is always vdo-next.
# - Modify the kernel_tree_repo_URL input argument to accept a path to an existing clone instead of
#   a URL. When a path is entered, bypass cloning the repo and use the repo at the path location.
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

ADDITIONAL_ARGS=()  # Array of command line arguments
COMMIT_SHAS=()      # Array of commit SHAs to be processed
CONFIG_MSG=()       # Array of strings to be used in the configuration message output to stdout
INPUT_TYPE=         # Indicated input type
OVERLAY_BRANCH=     # Name to give the kernel overlay branch being created
KERNEL_REPO_URL=    # URL for the kernel tree repository where the overlay branch will be created
KERNEL_BRANCH=      # Kernel tree branch to base the overlay on
PROCESS_INPUT_FX=   # Function to call to process the input(s)
SOURCE_BRANCH=      # Branch on the source repo where the relevant commits were made
MERGE_COMMIT=       # Merge commit to find commits to apply

# Expand the arguments provided into the necessary parameters to operate on.
process_args() {
  if [ -z "$1" ] || [ -z "$2" ] || [ -z "$3" ] || [ -z "$4" ] || [ -z "$5" ]; then
    printUsage
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
    "-b"|"--b"|"b")
      PROCESS_INPUT_FX=process_branch
      CONFIG_MSG=("branch" "Local branch to be used in overlay:")
      SOURCE_BRANCH=${ADDITIONAL_ARGS[0]}

      # Verify input branch exists
      git ls-remote --exit-code --heads --refs ${SOURCE_REPO_URL} ${SOURCE_BRANCH} &>/dev/null
      if [ $? == 2 ]; then
        echo -e "${COLOR_RED}ERROR: Branch ${SOURCE_BRANCH} not found on repo" \
                "$(get_repo_name ${SOURCE_REPO_URL})${NO_COLOR}"
        print_usage
      fi
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

  # Verify the kernel repo URL and branch are valid
  git ls-remote --exit-code ${KERNEL_REPO_URL} ${KERNEL_BRANCH} &>/dev/null
  return=$?
  if [ $return == 128 ]; then
    echo -e "${COLOR_RED}ERROR: Input url (${KERNEL_REPO_URL}) is not a valid repository${NO_COLOR}"
    print_usage
  elif [ $return == 2 ]; then
    echo -e "${COLOR_RED}ERROR: Branch ${KERNEL_BRANCH} not found on repo" \
            "$(get_repo_name ${KERNEL_REPO_URL})${NO_COLOR}"
    print_usage
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
  echo "${TOOL}: Create a kernel tree overlay branch from a commit, series"
  printf "%${indent}s%s\n" ' ' 'of commits, or local branch'
  echo
  echo "  Usage: "
  echo "  ./${TOOL} <kernel_tree_repo_URL> <kernel_tree_branch_name>"
  printf "%$((${indent}+3))s%s\n" ' ' '<overlay_branch_name> [ -c | -b | -m ] args...'
  echo
  echo "  Input Type Options:"
  echo "     -c  Process a commit or series of commits into a kernel overlay branch"
  echo "     -b  Process all changes on a local branch into a kernel overlay branch"
  echo "     -m  Process all changes merged in the named merge commit"
  echo
  echo "  Accepted Arguments:"
  echo "     Commit SHA(s) ordered from oldest to newest"
  echo "     Local branch name"
  echo
  echo "  ** Multiple arguments must be delimited with a space **"
  echo
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

  echo -en "\n$title_str\n"
  printf '=%.0s' $(seq 1 ${#title_str})
  echo # Intentional blank line
  echo "* Source repo: ${SOURCE_REPO_URL}"
  echo "* Source branch: ${SOURCE_BRANCH}"
  echo "* Kernel repo: ${KERNEL_REPO_URL}"
  echo "* Overlay branch to be created on kernel repo: ${OVERLAY_BRANCH}"
  echo "* Processing based on: ${CONFIG_MSG[0]}"
  echo "* ${CONFIG_MSG[1]}"

  for arg in ${ADDITIONAL_ARGS[@]}; do
    printf '%4s%s\n' '' "${arg}"
  done

  printf '=%.0s' $(seq 1 ${#title_str})
  echo -en "\n\n"
}

# Checkout the SOURCE_BRANCH if not already checked out
checkout_source_branch() {
  echo "Checking out the VDO source branch..."

  # Determine the source branch if unknown
  if [[ -z ${SOURCE_BRANCH} ]]; then
    SOURCE_BRANCH=$(git branch --show-current)

    # If a branch is not checked out, `git branch --show-current` will return an empty string
    if [[ -z ${SOURCE_BRANCH} ]]; then
      status=$(git status)
      branch_name="${status%% *}"
      SOURCE_BRANCH=$(git name-rev --name-only $branch_name)
    fi
  fi

  current_branch=$(git name-rev --name-only $(git branch --show-current))
  if [[ ! ${current_branch} =~ ${SOURCE_BRANCH} ]]; then
    # Fail if pending tracked/untracked changes are identified
    if [[ $(git status -s | wc -l) == 0 ]]; then
      git checkout ${SOURCE_BRANCH}
    else
      echo -e "${COLOR_RED}ERROR: Unable to checkout branch '${SOURCE_BRANCH}' - unclean tree"
      echo -e "Please resolve the issue and re-run this script${NO_COLOR}"
      exit
    fi
  else
    echo "Branch ${SOURCE_BRANCH} already checked out"
  fi
}

# Process the input commit SHA(s), verifying they are valid
process_commits() {
  echo -en "\nValidating input commit SHAs on branch '${SOURCE_BRANCH}'...\n"
  for commit in ${ADDITIONAL_ARGS[@]}; do
    git show ${commit} &>/dev/null

    if [ $? != 0 ]; then
      echo -e "${COLOR_RED}ERROR: Invalid commit SHA encountered ($commit)"
      echo -e "Please verify the input arguments and re-run this script${NO_COLOR}"
      exit
    fi

    # Compare the first 7 characters to find duplicates, as both full and partial SHAs may be input
    # Only the first valid entry will be kept
    if [[ ! ${COMMIT_SHAS[@]} =~ ${commit::7} ]]; then
      COMMIT_SHAS+=(${commit})
    else
      echo "Duplicate commit SHA found and removed ($commit)"
    fi
  done

  prompt_user_verify
}

# Process the input commit SHA(s), verifying they are valid
process_merge() {
  echo -en "\nValidating input commit SHA ($MERGE_COMMIT)...\n"
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

  # List all commits merged in this commit
  COMMIT_SHAS=($(git rev-list --reverse --no-merges ${MERGE_COMMIT}^2 ^${MERGE_COMMIT}^1))

  if [[ ${#COMMIT_SHAS[@]} == 0 ]]; then
    echo "No commits identified"
    exit
  fi

  prompt_user_verify
}

# Process the input branch, determining the applicable commit SHAs to be included in the patchset
process_branch() {
  echo -en "\nFinding relevant commits on branch '$SOURCE_BRANCH'...\n"

  # List all commits on SOURCE_BRANCH that are not on origin/HEAD
  COMMIT_SHAS=($(git rev-list --reverse --no-merges ${SOURCE_BRANCH} ^origin/HEAD))

  if [[ ${#COMMIT_SHAS[@]} == 0 ]]; then
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
      printf '%2s%s\n' '' "$(git show --no-patch --oneline ${sha})"
    done

    echo -en "\n"
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
        echo -en "${COLOR_RED}ERROR: Invalid response.\nPlease enter 'y' to proceed, or 'n' to" \
                 "exit.${NO_COLOR}\n\n"
        ;;
    esac
  done
}

# Prompt the user to push the overlay branch upstream
prompt_user_push() {
  user_input=""

  while [[ ! ${user_input} =~ ^(y|Y|m|M) ]]; do
    echo -en "\nPlease verify everything expected has been applied to overlay branch" \
             "${OVERLAY_BRANCH}.\n"
    echo "Ready to push ${OVERLAY_BRANCH} to $(get_repo_name ${KERNEL_REPO_URL})?"
    echo -en "[y - yes, push | n - no, delete | m - keep branch for manual push later]: "
    read user_input

    case "${user_input}" in
      "y"*|"Y"*)
        echo -en "Proceeding with push.\n\n"
        ;;
      "n"*|"N"*)
        exit
        ;;
      "m"*|"M"*)
        echo "Retaining ${LINUX_SRC} and branch ${OVERLAY_BRANCH} for manual push later."
        MANUAL_PUSH=0
        ;;
      *)
        echo -en "${COLOR_RED}ERROR: Invalid response.\nPlease enter 'y' to proceed with push," \
                 "'n' to delete the branch and exit,\nor 'm' to retain the branch for manual" \
                 "push at a later time.${NO_COLOR}\n\n"
        ;;
    esac
  done
}

# Cleanup artifacts
_cleanup() {
  cd $RUN_DIR
  echo -en "\nCleaning up and exiting\n"

  if [[ -d ${LINUX_SRC} ]] && [[ -d ${VDO_TREE} ]] && [[ ${DEBUG} != 0 ]]; then
    if [[ ${MANUAL_PUSH} != 0 ]]; then
      echo "Removing ${LINUX_SRC}"
      rm -rf ${LINUX_SRC}
    fi

    echo "Removing ${VDO_TREE}"
    rm -rf ${VDO_TREE}
  fi

  if [[ -e ${commit_file} ]] && [[ -f ${commit_file} ]]; then
    rm -fv ${commit_file}
  fi

  unset -f COMMIT_FILE DEBUG GITDATE GITAUTHOR LINUX_SRC
}

###############################################################################
# main()
trap _cleanup EXIT
test "$#" -lt 5 && print_usage

process_args $@
print_config
checkout_source_branch
$PROCESS_INPUT_FX

echo # Intentional blank line

# Store linux repo location as an absolute path in the current directory
LINUX_SRC=${RUN_DIR}/$(mktemp -d vdo-kernel-XXXXXX)
VDO_TREE=${RUN_DIR}/$(mktemp -d vdo-raw-XXXXXX)

# Shallow clone the specified dm-linux repo branch into LINUX_SRC
echo "Cloning kernel repo $(get_repo_name ${KERNEL_REPO_URL})/${KERNEL_BRANCH}"
git clone --branch ${KERNEL_BRANCH} --depth 1 ${KERNEL_REPO_URL} ${LINUX_SRC}
cd $LINUX_SRC
git checkout -b ${OVERLAY_BRANCH} ${KERNEL_BRANCH}

# Need to detect that our repo is x number of commits ahead of dm-vdo/vdo-devel
# Clone the current repo as ${VDO_TREE}? Is VDO_TREE even needed? Aren't we working from the clone when this script is called? Need to make a copy?
echo -en "\nCloning VDO tree $(get_repo_name ${SOURCE_REPO_URL})\n"
git clone $SOURCE_REPO_URL ${VDO_TREE}

# Loop over each commit in COMMIT_SHAS and overlay it on LINUX_SRC
for change in ${COMMIT_SHAS[@]}; do
  echo -en "\nProcessing changes from commit $change\n"

  cd ${VDO_TREE}

  # Clean up from the last build, if necessary
  if [[ "${change}" != "${COMMIT_SHAS[0]}" ]]; then
    echo -en "Cleaning up from the last build... "
    git clean -fdxq
    echo "Done"
  fi

  # Check out the new version to build and post - do we need to do this - it puts us in a detached HEAD state, so commits will not be retained
  #sudo git config --system advice.detachedHead "false"
  echo "Checking out the change to build and post it"
  git checkout ${change}

  # Capture the commit message from the change
  echo "Creating the initial commit file using the commit message from the change"
  commit_file=$(mktemp /tmp/overlay_commit_file.XXXXX)
  git log --format=%B -n 1 ${change} > ${commit_file}

  # Capture the source commit hash into the commit message as well
  echo "Original commit: ${change}" >> ${commit_file}

  # Capture authorship information for use later
  commit_author_email=$(git log --format=%aE -1 ${change})
  commit_author_name=$(git log --format=%aN -1 ${change})
  commit_date=$(git log --format=%ad -1 ${change})
  gitauthor="--author \"${commit_author_name} <${commit_author_email}>\""
  gitdate="--date \"${commit_date}\""

  # Build the necessary parts of the tree
  build_log_file=$(mktemp /tmp/overlay_build_log.XXXXX)
  echo -en "Building the VDO perl directory... "
  make -C src/perl >> ${build_log_file} 2>&1
  echo -en "Generating VDO statistics files... "
  make -C src/stats >> ${build_log_file} 2>&1
  echo "Done"

  if [[ $? != 0 ]]; then
    echo -e "${COLOR_RED}ERROR: Building the VDO tree failed. See ${build_log_file} for more" \
            "information.${NO_COLOR}"
    exit
  fi

  # Generate the kernel overlay and apply it to the linux tree
  echo "Generating the kernel overlay"
  cd src/packaging/kpatch

  export GITDATE="${gitdate}" GITAUTHOR="${gitauthor}" COMMIT_FILE="${commit_file}" \
         LINUX_SRC=${LINUX_SRC}
  make prepare kernel-overlay >> ${build_log_file}

  if [[ $? != 0 ]]; then
    echo -e "${COLOR_RED}ERROR: Generating the kernel overlay failed. See ${build_log_file} for" \
            "more information.${NO_COLOR}"
    exit
  fi

  echo "Applying the kernel overlay to branch '${OVERLAY_BRANCH}'"
  make kernel-kpatch >> ${build_log_file} 2>&1

  if [[ $? != 0 ]]; then
    return=$(tail -5 ${build_log_file} | grep "^[nN]othing to commit" | wc -l)

    if [[ $return > 0 ]]; then
      echo "Nothing to apply - moving on."
    else
      echo -e "${COLOR_RED}ERROR: Applying the kernel overlay to branch '${OVERLAY_BRANCH}'" \
              "failed. See ${build_log_file} for more information.${NO_COLOR}"
      exit
    fi
  fi

  # Clean up
  echo "Cleaning up build artifacts"

  rm -fv ${commit_file}

  if [[ ${DEBUG} != 0 ]]; then
    rm -fv ${build_log_file}
  fi

  unset -f GITDATE GITAUTHOR COMMIT_FILE
done

# Push the kernel overlay branch to the remote kernel repository
cd ${LINUX_SRC}
prompt_user_push

if [[ ${MANUAL_PUSH} != 0 ]]; then
  git push -u origin ${OVERLAY_BRANCH}
fi

exit 0
