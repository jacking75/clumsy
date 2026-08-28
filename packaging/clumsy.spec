# RPM spec for clumsy  (Phase 4.6)
#
# Build with:
#   make package-rpm
# or by hand:
#   rpmbuild -bb --define "_topdir $(pwd)/rpmbuild" packaging/clumsy.spec
#
# Note: unlike the .deb, this spec has not been build-tested in this repository -
# no rpm toolchain was available on the development machine. It follows the same
# layout as the deb package and is expected to work, but treat the first build on
# a Fedora/RHEL host as verification.

Name:           clumsy
Version:        0.4
Release:        1%{?dist}
Summary:        Controllable network condition simulator

License:        MIT
URL:            https://github.com/jacking75/clumsy
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc-c++ >= 16
BuildRequires:  make
BuildRequires:  libnetfilter_queue-devel
BuildRequires:  libmnl-devel
Requires:       libnetfilter_queue
Requires:       libmnl
Requires:       iptables
Recommends:     libcap

%description
clumsy intercepts live network packets and applies lag, jitter, packet loss,
reordering, duplication, payload tampering, TCP resets and bandwidth caps to
them, then re-injects them. Useful for reproducing poor network conditions
while testing an application.

On Linux it captures through NFQUEUE, so a matching iptables rule decides which
traffic reaches it. clumsy can install and remove those rules itself with
--auto-iptables.

Control it from the built-in web dashboard, the REST API, the Unix control
socket at /run/clumsy.sock, or command line arguments.

%prep
%autosetup

%build
make %{?_smp_mflags}

%install
install -D -m 0755 bin/linux/clumsy        %{buildroot}%{_bindir}/clumsy
install -D -m 0644 etc/config.json         %{buildroot}%{_datadir}/clumsy/config.json
install -D -m 0644 etc/config.txt          %{buildroot}%{_datadir}/clumsy/config.txt
install -D -m 0644 etc/web/index.html      %{buildroot}%{_datadir}/clumsy/web/index.html
install -D -m 0644 etc/scenario-example.json %{buildroot}%{_datadir}/clumsy/scenario-example.json
install -D -m 0644 docs/LINUX.md           %{buildroot}%{_docdir}/clumsy/LINUX.md
install -D -m 0644 README.md               %{buildroot}%{_docdir}/clumsy/README.md

# clumsy resolves config.json and web/ relative to its own executable, so the
# shared data has to be reachable from %{_bindir}.
mkdir -p %{buildroot}%{_bindir}/web
ln -sf %{_datadir}/clumsy/web/index.html   %{buildroot}%{_bindir}/web/index.html
ln -sf %{_datadir}/clumsy/config.json      %{buildroot}%{_bindir}/config.json

%post
# Same rationale as the deb postinst: file capabilities rather than setuid root.
if command -v setcap >/dev/null 2>&1; then
    setcap cap_net_admin,cap_net_raw+ep %{_bindir}/clumsy 2>/dev/null || \
        echo "clumsy: could not set capabilities; run clumsy with sudo instead" >&2
fi

%files
%{_bindir}/clumsy
%{_bindir}/web/index.html
%{_bindir}/config.json
%{_datadir}/clumsy/
%doc %{_docdir}/clumsy/

%changelog
* Fri Aug 28 2026 clumsy contributors <noreply@example.com> - 0.4-1
- Linux support: NFQUEUE capture backend, filter expression evaluator,
  --auto-iptables, Unix control socket, capability-based privileges
