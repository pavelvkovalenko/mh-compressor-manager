Name:           mh-compressor-manager
Version:        1.0.0
Release:        1%{?dist}
Summary:        Автоматическое сжатие статического контента для nginx

License:        MIT License
URL:            https://mediahive.ru
BuildArch:      x86_64

Source0:        %{name}-%{version}.tar.gz

# Зависимости для сборки
BuildRequires:  cmake >= 3.20
BuildRequires:  gcc-c++ >= 13
BuildRequires:  zlib-ng-compat-devel
BuildRequires:  brotli-devel
BuildRequires:  systemd-devel
BuildRequires:  libselinux-devel
BuildRequires:  pkgconf-pkg-config

# Зависимости для выполнения (ИСПРАВЛЕНО для Fedora 43)
Requires:       zlib-ng-compat
Requires:       libbrotli
Requires:       systemd
Requires:       libselinux

%description
mh-compressor-manager — фоновая служба для автоматического сжатия 
статического контента веб-сервера nginx.

%prep
%setup -q -n %{name}-%{version}

%build
rm -rf build CMakeCache.txt CMakeFiles
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
make -j$(nproc)

%install
cd build
make install DESTDIR=%{buildroot}

mkdir -p %{buildroot}/etc/mediahive
install -m 644 ../compressor-manager.conf %{buildroot}/etc/mediahive/

mkdir -p %{buildroot}/%{_unitdir}
install -m 644 ../mh-compressor-manager.service %{buildroot}/%{_unitdir}/

%post
if [ $1 -eq 1 ]; then
    /usr/bin/systemctl daemon-reload >/dev/null 2>&1 || :
fi

%preun
if [ $1 -eq 0 ]; then
    /usr/bin/systemctl stop mh-compressor-manager >/dev/null 2>&1 || :
fi

%postun
if [ $1 -ge 1 ]; then
    /usr/bin/systemctl daemon-reload >/dev/null 2>&1 || :
fi

%files
/usr/bin/mh-compressor-manager
%config(noreplace) /etc/mediahive/compressor-manager.conf
%{_unitdir}/mh-compressor-manager.service
%doc README.md
%doc README.html
%license LICENSE

%changelog
* Mon Jan 01 2026 Pavel Kovalenko <dev@mediahive.ru> - 1.0.0-1
- Initial package release для Fedora 43
- © 2026 MediaHive.ru, ООО ОКБ "Улей"
