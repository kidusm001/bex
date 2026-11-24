Name:           bex
Version:        1.0.0
Release:        1%{?dist}
Summary:        Native Linux performance monitor built with Qt 6

License:        MIT
URL:            https://github.com/kidusm001/bex
Source0:        %{url}/releases/download/v%{version}/%{name}-%{version}.tar.gz

BuildRequires:  cmake >= 3.21
BuildRequires:  desktop-file-utils
BuildRequires:  gcc-c++
BuildRequires:  ninja-build
BuildRequires:  pkgconf-pkg-config
BuildRequires:  pkgconfig(xcb)
BuildRequires:  pkgconfig(xcb-res)
BuildRequires:  cmake(Qt6Core) >= 6.4
BuildRequires:  cmake(Qt6Gui) >= 6.4
BuildRequires:  cmake(Qt6Widgets) >= 6.4

Requires:       hicolor-icon-theme
Recommends:     qt6-qtwayland

%description
bex is a native Linux performance monitor built with C++20 and Qt 6 Widgets.
It reads /proc, /sys, and hwmon directly with no root privileges or helper
daemons, showing live graphs and readings for CPU (total and per-core),
memory and swap, disks and filesystems, network interfaces, hardware
temperatures, and grouped processes.

%prep
%autosetup -n %{name}-%{version}

%build
%cmake -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DBEX_BUILD_TESTS=OFF \
    -DBEX_X11=ON
%cmake_build

%install
%cmake_install

%check
desktop-file-validate %{buildroot}%{_datadir}/applications/io.github.kidusm001.bex.desktop

%files
%license LICENSE
%doc README.md
%{_bindir}/bex
%{_datadir}/applications/io.github.kidusm001.bex.desktop
%{_datadir}/icons/hicolor/scalable/apps/io.github.kidusm001.bex.svg
%{_datadir}/metainfo/io.github.kidusm001.bex.metainfo.xml

%changelog
* Mon Nov 24 2026 Kidus Mesfin <kidusmesfin27@gmail.com> - 1.0.0-1
- Initial RPM package for bex
