# vdo-devel

Primary development repository for the dm-vdo kernel module and related
userspace tools, plus documentation, tests and packaging support for dm-vdo.

## Background

VDO is a device-mapper target that provides inline block-level deduplication,
compression, and thin provisioning capabilities for primary storage. VDO
is managed through LVM and can be integrated into any existing storage stack.

Deduplication is a technique for reducing the consumption of storage resources
by eliminating multiple copies of duplicate blocks. Compression takes the
individual unique blocks and shrinks them with coding algorithms; these reduced
blocks are then efficiently packed together into physical blocks. Thin
provisioning manages the mapping from logical block addresses presented by VDO
to where the data has actually been stored, and also eliminates any blocks of
all zeroes.

With deduplication, instead of writing the same data more than once each
duplicate block is detected and recorded as a reference to the original
block. VDO maintains a mapping from logical block addresses (presented to the
storage layer above VDO) to physical block addresses on the storage layer
under VDO. After deduplication, multiple logical block addresses may be mapped
to the same physical block address; these are called shared blocks and are
reference-counted by the software.

With VDO's compression, blocks are compressed with the fast LZ4 algorithm, and
collected together where possible so that multiple compressed blocks fit within
a single 4 KB block on the underlying storage. Each logical block address is
mapped to a physical block address and an index within it for the desired
compressed data. All compressed blocks are individually reference-counted for
correctness.

Block sharing and block compression are invisible to applications using the
storage, which read and write blocks as they would if VDO were not present.
When a shared block is overwritten, a new physical block is allocated for
storing the new block data to ensure that other logical block addresses that
are mapped to the shared physical block are not modified.

## History

VDO was originally developed by Permabit Technology Corp. as a proprietary set
of kernel modules and userspace tools. This software and technology has been
acquired by Red Hat and relicensed under the GPL (v2 or later). The kernel
module has been merged into the upstream Linux kernel as the dm-vdo device
mapper target. The source for this module can be found in drivers/md/dm-vdo/.

## Documentation

Documentation can be found in the `doc` directory. Additional documentation
can be found here:

- [RHEL9 VDO Documentation](https://access.redhat.com/documentation/en-us/red_hat_enterprise_linux/9/html/deduplicating_and_compressing_logical_volumes_on_rhel/index)
- [RHEL8 VDO Documentation](https://access.redhat.com/documentation/en-us/red_hat_enterprise_linux/8/html/deduplicating_and_compressing_storage/index)
- [RHEL7 VDO Integration Guide](https://access.redhat.com/documentation/en-us/red_hat_enterprise_linux/7/html/storage_administration_guide/vdo-integration)
- [RHEL7 VDO Evaluation Guide](https://access.redhat.com/documentation/en-us/red_hat_enterprise_linux/7/html/storage_administration_guide/vdo-evaluation)

## Releases

This repository is the upstream source for the dm-vdo kernel module. The most
recent updates enter the mainline Linux kernel through the 
[device-mapper tree](https://git.kernel.org/pub/scm/linux/kernel/git/device-mapper/linux-dm.git/).

Userspace tools for managing dm-vdo device-mapper targets are published
periodically through the [vdo](https://github.com/dm-vdo/vdo) repository.

## Building

In order to build the entire project, invoke the following command from the
`src` directory of this tree:

        make all archive

Source tarballs and installable RPMs will appear in the `archive` directory.

Patched sources that work with some older kernels can be found
[here](https://github.com/rhawalsh/kvdo).

## Communication Channels and Contributions

Community feedback, participation and patches are welcome. Code contributions
can be filed as pull requests and will be accepted based on review feedback
and testing results. Patches can also be submitted to dm-devel@lists.linux.dev
using the normal kernel patch submission process.

Questions and suggestions can be filed as issues. We try to address issues and
pull requests in a timely manner, but a response may sometimes take a few
days. Your patience is appreciated.

## Licensing

[GPL v2.0 or later](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html).
All contributions retain ownership by their original author, but must also
be licensed under the GPL 2.0 or later to be merged.

