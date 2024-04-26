%define spec_release 33
%define bash_completions_dir %{_datadir}/bash-completion/completions

Summary: Management tools for Virtual Data Optimizer
Name: vdo
Version: @VERSION@
Release: %{spec_release}%{?dist}

License: GPL-2.0-only
URL: http://github.com/dm-vdo/vdo
Source0: %{name}-%{version}.tgz

Requires: kmod-kvdo >= 8.2
ExcludeArch: s390
ExcludeArch: ppc
ExcludeArch: ppc64
ExcludeArch: i686
BuildRequires: device-mapper-devel
BuildRequires: device-mapper-event-devel
BuildRequires: gcc
BuildRequires: libblkid-devel
BuildRequires: libuuid-devel
BuildRequires: make
%ifarch %{valgrind_arches}
BuildRequires: valgrind-devel
%endif
BuildRequires: zlib-devel

# Disable an automatic dependency due to a file in examples/monitor.
%define __requires_exclude perl

%description
Virtual Data Optimizer (VDO) is a device mapper target that delivers
block-level deduplication, compression, and thin provisioning.

This package provides the user-space management tools for VDO.

%package support
Summary: Support tools for Virtual Data Optimizer
License: GPL-2.0-only

Requires: libuuid >= 2.23
ExcludeArch: s390
ExcludeArch: ppc
ExcludeArch: ppc64
ExcludeArch: i686

%description support
Virtual Data Optimizer (VDO) is a device mapper target that delivers
block-level deduplication, compression, and thin provisioning.

This package provides the user-space support tools for VDO.

%prep
%setup -q

%build
make

%install
make install DESTDIR=$RPM_BUILD_ROOT INSTALLOWNER= name=%{name} bindir=%{_bindir} \
  mandir=%{_mandir} defaultdocdir=%{_defaultdocdir} libexecdir=%{_libexecdir} \
  presetdir=%{_presetdir} python3_sitelib=/%{python3_sitelib} \
  sysconfdir=%{_sysconfdir} unitdir=%{_unitdir}

%files
%license COPYING
%{_bindir}/vdodmeventd
%{_bindir}/vdodumpconfig
%{_bindir}/vdoforcerebuild
%{_bindir}/vdoformat
%{_bindir}/vdosetuuid
%{_bindir}/vdostats
%{bash_completions_dir}/vdostats
%dir %{_defaultdocdir}/%{name}
%dir %{_defaultdocdir}/%{name}/examples
%dir %{_defaultdocdir}/%{name}/examples/monitor
%doc %{_defaultdocdir}/%{name}/examples/monitor/monitor_check_vdostats_logicalSpace.pl
%doc %{_defaultdocdir}/%{name}/examples/monitor/monitor_check_vdostats_physicalSpace.pl
%doc %{_defaultdocdir}/%{name}/examples/monitor/monitor_check_vdostats_savingPercent.pl
%{_mandir}/man8/vdodmeventd.8*
%{_mandir}/man8/vdodumpconfig.8*
%{_mandir}/man8/vdoforcerebuild.8*
%{_mandir}/man8/vdoformat.8*
%{_mandir}/man8/vdosetuuid.8*
%{_mandir}/man8/vdostats.8*

%files support
%{_bindir}/adaptlvm
%{_bindir}/vdoaudit
%{_bindir}/vdodebugmetadata
%{_bindir}/vdodumpblockmap
%{_bindir}/vdodumpmetadata
%{_bindir}/vdolistmetadata
%{_bindir}/vdoreadonly
%{_bindir}/vdorecover
%{_bindir}/vdoregenerategeometry
%{_mandir}/man8/adaptlvm.8*
%{_mandir}/man8/vdoaudit.8*
%{_mandir}/man8/vdodebugmetadata.8*
%{_mandir}/man8/vdodumpblockmap.8*
%{_mandir}/man8/vdodumpmetadata.8*
%{_mandir}/man8/vdolistmetadata.8*
%{_mandir}/man8/vdoreadonly.8*
%{_mandir}/man8/vdorecover.8*
%{_mandir}/man8/vdoregenerategeometry.8*

%changelog
* Fri Apr 26 2024 - Susan LeGendre-McGhee <slegendr@redhat.com> - 8.4.0.0-33
- Reformat to improve clarity and make consistent with current standards.

* Fri Apr 14 2023 - Susan LeGendre-McGhee <slegendr@redhat.com> - 8.1.0.0-32
- Removed exclusion of other supported architectures.

* Wed Jan 04 2023 - Joe Shimkus <jshimkus@redhat.com> - 8.1.0.0-31
- Enabled building on aarch64.

* Fri Jul 15 2022 - Andy Walsh <awalsh@redhat.com> - 8.1.0.0-30
- Added vdorecover utility.

* Tue May 10 2022 - Bruce Johnston <bjohnsto@redhat.com> - 8.1.0.0-29
- Removed ansible examples.

* Thu Jan 27 2022 - Andy Walsh <awalsh@redhat.com> - 8.1.0.0-28
- Added adaptLVMVDO.

* Wed Aug 04 2021 - Andy Walsh <awalsh@redhat.com> - 8.1.0.0-27
- Removed all python code and associated dependencies.
- Added C-based vdostats.

* Wed Oct 21 2020 - Sweet Tea Dorminy <sweettea@redhat.com> - 6.2.0.0-26
- Updated to include systemd/udev rules and services to improve startup reliability.

* Thu May 14 2020 - Andy Walsh <awalsh@redhat.com> - 6.2.0.0-25
- Created vdo-support package

* Wed May 13 2020 - Michael Sclafani <sclafani@redhat.com> - 6.2.0.0-24
- Added vdoreadonly manpage to %files.

* Tue May 12 2020 - Michael Sclafani <sclafani@redhat.com> - 6.2.0.0-23
- Added vdolistmetadata to %files.

* Tue May 12 2020 - Michael Sclafani <sclafani@redhat.com> - 6.2.0.0-22
- Added vdoregenerategeometry to %files.

* Fri May 08 2020 - Michael Sclafani <sclafani@redhat.com> - 6.2.0.0-21
- Added vdodebugmetadata to %files.

* Wed May 06 2020 - Michael Sclafani <sclafani@redhat.com> - 6.2.0.0-20
- Added vdodumpblockmap to %files.

* Wed May 06 2020 - Michael Sclafani <sclafani@redhat.com> - 6.2.0.0-19
- Added vdodumpmetadata to %files.

* Mon May 04 2020 - Michael Sclafani <sclafani@redhat.com> - 6.2.0.0-18
- Added vdoaudit to %files.

* Fri Feb 15 2019 - Bryan Gurney <bgurney@redhat.com> - 6.2.0.0-17
- Removed VDO Ansible module file.

* Fri Aug 03 2018 - Andy Walsh <awalsh@redhat.com> - 6.2.0.0-16
- Fixed to work with different versions of python
- Added kmod-kvdo dependency

* Mon Jul 30 2018 - Bryan Gurney <bgurney@redhat.com> - 6.2.0.0-15
- Transformed nagios scripts into monitor scripts.

* Thu Jun 28 2018 - Bruce Johnston <bjohnsto@redhat.com> - 6.2.0.0-14
- Added build requirement for libdevmapper-event-devel

* Thu Jun 21 2018 - Andy Walsh <awalsh@redhat.com> - 6.2.0.0-13
- Added the dist tag

* Wed Jun 13 2018 - Bruce Johnston <bjohnsto@redhat.com> - 6.2.0.0-12
- Add vdodmeventd to %files.

* Tue May 22 2018 - Joseph Chapman <jochapma@redhat.com> - 6.2.0.0-11
- Convert to Python 3.

* Thu May 17 2018 - Joseph Chapman <jochapma@redhat.com> - 6.2.0.0-10
- Added vdo/__init__.py to %files.

* Fri Mar 09 2018 - Joseph Shimkus <jshimkus@redhat.com> - 6.2.0.0-9
- Added new vdomgmnt module to %files.

* Wed Mar 07 2018 - Joseph Chapman <jochapma@redhat.com> - 6.2.0.0-8
- Removed python files no longer in the distribution.

* Thu Feb 22 2018 - Joseph Chapman <jochapma@redhat.com> - 6.2.0.0-7
- Use systemd macros in spec file.

* Wed Feb 14 2018 - Joseph Chapman <jochapma@redhat.com> - 6.2.0.0-6
- Added new vdomgmnt module to %files.

* Fri Feb 09 2018 - Sweet Tea Dorminy <sweettea@redhat.com> - 6.2.0.0-5
- Added support for Fedora 27.

* Tue Feb 06 2018 - Andy Walsh <awalsh@redhat.com> - 6.2.0.0-4
- Updated summary and description

* Wed Jan 31 2018 - Joseph Chapman <jochapma@redhat.com> - 6.2.0.0-3
- Fix URL to point to GitHub tree

* Mon Jan 08 2018 - Joseph Chapman <jochapma@redhat.com> - 6.2.0.0-2
- Enable and disable systemctl unit scripts on install/remove

* Fri Oct 27 2017 - Joseph Chapman <jochapma@redhat.com> - 6.2.0.0-1
- New VDO minor version
- Don't let make install try to set file ownerships itself

* Thu Oct 12 2017 - Joseph Chapman <jochapma@redhat.com> - 6.1.0.0-6
- Added new man pages

* Fri Oct  6 2017 - Joseph Chapman <jochapma@redhat.com> - 6.1.0.0-5
- Fixed a typo in the package description
- Fixed man page paths

* Thu Oct  5 2017 - Joseph Chapman <jochapma@redhat.com> - 6.1.0.0-4
- Fix vdostats name in nagios examples
- Build only on the x86_64 architecture
- Add systemd files

* Thu Oct  5 2017 - Joseph Chapman <jochapma@redhat.com> - 6.1.0.0-3
- Added missing Build-Requires and incorporated naming changes

* Wed Oct  4 2017 - Joseph Chapman <jochapma@redhat.com> - 6.1.0.0-2
- Fixed requirements and tags in %files section

* Tue Oct  3 2017 - Joseph Chapman <jochapma@redhat.com> - 6.1.0.0-1
- Initial implementation
