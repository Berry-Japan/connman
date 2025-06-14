%define name connman
%define version 1.44
%define release b1
%define _libdir /usr/lib

Name:		%{name}
Version:	%{version}
Release:	%{release}
Summary:	Connection Manager
License:	GPLv2
Group:		System Environment/Networking
URL:		https://01.org/connman
Source0:	https://www.kernel.org/pub/linux/network/connman/%{name}-%{version}.tar.xz
Buildroot:	%{_tmppath}/%{name}-%{version}

Requires:	iwd
BuildRequires:	dbus-devel, iptables-devel
BuildRequires:	libmnl-devel
BuildRequires:	readline-devel
BuildRequires:	kernel-headers

%description
Connection Manager provides a daemon for managing Internet connections
within embedded devices running the Linux operating system.


%prep
%setup -q
#./configure --prefix=/usr --disable-debug --enable-iwd --disable-wispr --enable-nmcompat --enable-polkit
#export CFLAGS="-fPIE"
#export LDFLAGS="-pie"
#define _hardened_build 0
#sudo sysctl -w kernel.randomize_va_space=0
#configure	--disable-debug \
./configure	--prefix=/usr \
                --disable-debug \
		--enable-iwd \
		--disable-wispr \
		--enable-nmcompat \
		--enable-polkit \
		--disable-static \
		--enable-ethernet \
		--enable-wifi \
		--enable-udhcp \
		--enable-bluetooth \
		--enable-loopback \
		--enable-dnsproxy \
		--enable-resolvconf \
		--enable-udev \
		--enable-client \
		--enable-threads \
		--enable-google \
		 CFLAGS=-Os

%build
%make_build

%install
%make_install
strip $RPM_BUILD_ROOT/usr/sbin/connmand


%clean
[ -n "%{buildroot}" -a "%{buildroot}" != / ] && rm -rf %{buildroot}
rm -rf $RPM_BUILD_DIR/%{name}-%{version}


%pre
#service_add_pre connman.service
#service_add_pre connman-vpn.service
#service_del_postun connman-wait-online.service

%post
#service_add_post connman.service
#service_add_post connman-vpn.service
#service_del_postun connman-wait-online.service
#tmpfiles_create %{_tmpfilesdir}/connman.conf
#tmpfiles_create %{_tmpfilesdir}/connman_resolvconf.conf


%preun
#service_del_preun connman.service
#service_del_preun connman-vpn.service
#service_del_preun connman-wait-online.service

%postun
#service_del_postun connman.service
#service_del_postun connman-vpn.service
#service_del_postun connman-wait-online.service


%files
#%defattr (-,root,root)
%{_bindir}/connmanctl
%{_sbindir}/connmand
%{_sbindir}/connman-vpnd
%{_sbindir}/connmand-wait-online
%{_tmpfilesdir}/connman_resolvconf.conf
%dir %{_libdir}/%{name}
%dir %{_libdir}/%{name}/scripts
%dir %{_libdir}/%{name}/plugins-vpn
%{_datadir}/dbus-1/system.d/connman.conf
%{_datadir}/dbus-1/system.d/connman-nmcompat.conf
%{_datadir}/dbus-1/system.d/connman-vpn-dbus.conf
%{_datadir}/dbus-1/system-services/net.connman.vpn.service
%{_unitdir}/connman.service
%{_unitdir}/connman-vpn.service
%{_unitdir}/connman-wait-online.service
%ghost %dir %{_localstatedir}/lib/%{name}
%ghost %dir %{_localstatedir}/lib/%{name}-vpn
%ghost %{_localstatedir}/lib/%{name}/settings

%{_libdir}/connman/plugins-vpn/wireguard.so

%{_datadir}/polkit-1/actions/net.connman.policy
%{_datadir}/polkit-1/actions/net.connman.vpn.policy


%changelog
* Wed Jun 11 2025 Yuichiro Nakada <berry@berry-lab.net>
- Update to 1.44
* Thu Nov 23 2023 Yuichiro Nakada <berry@berry-lab.net>
- Create for Berry Linux
