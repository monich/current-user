Name:     current-user
Summary:  Run program as current user
Version:  1.0.0
Release:  1
License:  BSD
URL:      https://github.com/sailfishos/current-user
Source0:  %{name}-%{version}.tar.bz2

%define glib_version 2.4
%define libdbusaccess_version 1.0.13

Requires: glib2 >= %{glib_version}
Requires: libdbusaccess >= %{libdbusaccess_version}

BuildRequires: pkgconfig(glib-2.0) >= %{glib_version}
BuildRequires: pkgconfig(libdbusaccess) >= %{libdbusaccess_version}
BuildRequires: pkgconfig(libglibutil)

%description
Utility for running a program as the current Sailfish OS user.

%prep
%setup -q -n %{name}-%{version}

%build
make KEEP_SYMBOLS=1 release

%install
rm -rf %{buildroot}
make DESTDIR=%{buildroot} install

%files
%defattr(-,root,root,-)
%{_bindir}/current-user
