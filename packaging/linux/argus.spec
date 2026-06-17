# RPM spec for Argus (Fedora / RHEL / Alma / Rocky).
# Build with: rpmbuild -ba argus.spec  (see build-rpm.sh for a turnkey wrapper)
Name:           argus
Version:        %{?_argus_version}%{!?_argus_version:1.0.0}
Release:        1%{?dist}
Summary:        Audio master QA - defect, loudness and true-peak analysis

License:        GPL-3.0-or-later
URL:            https://example.com/argus
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  cmake gcc-c++ libsndfile-devel mesa-libGL-devel pkgconfig
BuildRequires:  libXrandr-devel libXinerama-devel libXcursor-devel libXi-devel
BuildRequires:  wayland-devel libxkbcommon-devel wayland-protocols-devel libdecor-devel
Requires:       libsndfile mesa-libGL libwayland-client libxkbcommon

%description
Argus catches dropouts, glitches, clipping and loudness/true-peak violations in
delivered audio masters, with an RX-style spectrogram and PDF/CSV/JSON reports.
Ships a desktop app (argus-gui) and a headless CLI (argus) for CI gating.

%prep
%autosetup -n %{name}-%{version}

%build
%cmake -DARGUS_BUILD_GUI=ON -DCMAKE_BUILD_TYPE=Release
%cmake_build

%install
%cmake_install

%files
%license LICENSE
%{_bindir}/argus
%{_bindir}/argus-gui
%{_datadir}/argus/resources/*
%{_datadir}/applications/argus.desktop
%{_datadir}/pixmaps/argus.png

%changelog
* Tue Jun 17 2026 Argus <argus@localhost> - 1.0.0-1
- Initial package.
