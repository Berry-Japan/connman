%define name connman
%define version 1.41
%define release b1
%define _libdir /usr/lib

Name:		%{name}
Version:	%{version}
Release:	%{release}
Summary:	Connection Manager
License:	GPL-2.0-only
Group:		System/Daemons
URL:		http://www.moblin.org/
Source0:	http://www.kernel.org/pub/linux/network/connman/connman-%{version}.tar.bz2
Buildroot:	%{_tmppath}/%{name}-%{version}

Requires:	iwd
BuildRequires:	iptables-devel
BuildRequires:	libmnl-devel
BuildRequires:	readline-devel
BuildRequires:	kernel-headers


%description
Connection Manager provides a daemon for managing Internet connections
within embedded devices running the Linux operating system.


##
## Setup Section
##

%prep
%setup -q
./configure --prefix=/usr --disable-debug --enable-iwd --disable-wispr --enable-nmcompat --enable-polkit


##
## Build Section
##

%build
%make_build


##
## Install Section
##

%install
%make_install


##
## Clean Section
##

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


##
## Files Section
##

%files
%defattr (-,root,root)
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


##
## change log
##

%changelog
* Thu Nov 23 2023 Yuichiro Nakada <berry@berry-lab.net>
- Create for Berry Linux
