# This file is made by Fredrik Rambris <fredrik@rambirs.com>

Name: moc
Summary: Music on Console - Console audio player for Linux/UNIX
Version: 2.4.2
Release: 2
License: GPL
Group: Applications/Multimedia
URL: http://moc.draper.net
Source: ftp://ftp.daper.net/pub/soft/moc/stable/moc-%{version}.tar.bz2
BuildRoot: %{_tmppath}/%{name}-%{version}-%{release}-root
Requires: ncurses alsa-lib
BuildRequires: ncurses-devel alsa-lib-devel

%if %{!?_without_samplerate:1}0
BuildRequires: libsamplerate-devel
Requires: libsamplerate
%endif

%if %{!?_without_curl:1}0
BuildRequires: curl-devel
Requires: curl
%endif

%if %{!?_without_jack:1}0
BuildRequires: libjack-devel
Requires: libjack
%endif

%if %{!?_without_rcc:1}0
BuildRequires: librcc-devel
Requires: librcc
%endif

%description
MOC (music on console) is a console audio player for LINUX/UNIX designed to be
powerful and easy to use. You just need to select a file from some directory
using the menu similar to Midnight Commander, and MOC will start playing all
files in this directory beginning from the chosen file.

%if %{!?_without_mp3:1}0
%package mp3
Summary: MP3 decoder for MoC - Music on Console
Group: Applications/Multimedia
BuildRequires: libmad-devel libid3tag-devel
Requires: libmad libid3tag
Requires: %{name} = %{version}
%description mp3
MOC (music on console) is a console audio player for LINUX/UNIX designed to be
powerful and easy to use. You just need to select a file from some directory
using the menu similar to Midnight Commander, and MOC will start playing all
files in this directory beginning from the chosen file.

This package contains the MP3 decoder
%endif

%if %{!?_without_musepack:1}0
%package musepack
Summary: Musepack (MPC) decoder for MoC - Music on Console
Group: Applications/Multimedia
BuildRequires: libmpcdec-devel taglib-devel
Requires: libmpcdec taglib
Requires: %{name} = %{version}
%description musepack
MOC (music on console) is a console audio player for LINUX/UNIX designed to be
powerful and easy to use. You just need to select a file from some directory
using the menu similar to Midnight Commander, and MOC will start playing all
files in this directory beginning from the chosen file.

This package contains the Musepack (MPC) decoder
%endif

%if %{!?_without_vorbis:1}0
%package vorbis
Summary: Ogg decoder for MoC - Music on Console
Group: Applications/Multimedia
BuildRequires: libogg-devel libvorbis-devel
Requires: libogg libvorbis
Requires: %{name} = %{version}
Obsoletes: ogg
%description vorbis
MOC (music on console) is a console audio player for LINUX/UNIX designed to be
powerful and easy to use. You just need to select a file from some directory
using the menu similar to Midnight Commander, and MOC will start playing all
files in this directory beginning from the chosen file.

This package contains the Ogg Vorbis decoder
%endif

%if %{!?_without_flac:1}0
%package flac
Summary: FLAC decoder for MoC - Music on Console
Group: Applications/Multimedia
BuildRequires: flac-devel
Requires: flac
Requires: %{name} = %{version}
%description flac
MOC (music on console) is a console audio player for LINUX/UNIX designed to be
powerful and easy to use. You just need to select a file from some directory
using the menu similar to Midnight Commander, and MOC will start playing all
files in this directory beginning from the chosen file.

This package contains the FLAC decoder
%endif

%if %{!?_without_sndfile:1}0
%package sndfile
Summary: Decoder of the sndfile formats for MoC - Music on Console
Group: Applications/Multimedia
BuildRequires: libsndfile-devel
Requires: libsndfile
Requires: %{name} = %{version}
%description sndfile
MOC (music on console) is a console audio player for LINUX/UNIX designed to be
powerful and easy to use. You just need to select a file from some directory
using the menu similar to Midnight Commander, and MOC will start playing all
files in this directory beginning from the chosen file.

This package contains the decoders of sndfile
%endif

%if %{!?_without_speex:1}0
%package speex
Summary: Speex decoder for MoC - Music on Console
Group: Applications/Multimedia
BuildRequires: speex-devel
Requires: speex
Requires: %{name} = %{version}
%description speex
MOC (music on console) is a console audio player for LINUX/UNIX designed to be
powerful and easy to use. You just need to select a file from some directory
using the menu similar to Midnight Commander, and MOC will start playing all
files in this directory beginning from the chosen file.

This package contains the Speex decoder
%endif

%if %{!?_without_ffmpeg:1}0
%package ffmpeg
Summary: FFMPEG (WMA, Real etc.) decoder for MoC - Music on Console
Group: Applications/Multimedia
BuildRequires: ffmpeg-devel
Requires: ffmpeg
Requires: %{name} = %{version}
%description ffmpeg
MOC (music on console) is a console audio player for LINUX/UNIX designed to be
powerful and easy to use. You just need to select a file from some directory
using the menu similar to Midnight Commander, and MOC will start playing all
files in this directory beginning from the chosen file.

This package contains the FFMPEG (WMA, Real etc.) decoder
%endif

%prep
%setup -q

%build
%configure \
		%{?_without_mp3:--without-mp3} \
		%{?_without_musepack:--without-musepack} \
		%{?_without_vorbis:--without-vorbis} \
		%{?_without_flac:--without-flac} \
		%{?_without_sndfile:--without-sndfile} \
		%{?_without_speex:--without-speex} \
		%{?_without_samplerate:--without-samplerate} \
		%{?_without_curl:--without-curl} \
		%{?_without_ffmpeg:--without-ffmpeg} \
		%{?_without_jack:--without-jack} \
		%{?_without_rcc:--without-rcc} \
		--disable-debug
%{__make} %{?_smp_mflags}

%install
[ -n "$RPM_BUILD_ROOT" -a "$RPM_BUILD_ROOT" != / ] && rm -rf "$RPM_BUILD_ROOT"
%makeinstall
%{__rm} -rf $RPM_BUILD_ROOT/usr/share/doc/moc
mkdir -p $RPM_BUILD_ROOT%_libdir/moc/decoder_plugins
mv $RPM_BUILD_ROOT%_libdir/*.so $RPM_BUILD_ROOT%_libdir/moc/decoder_plugins
rm -f $RPM_BUILD_ROOT%_libdir/*.la

%clean
[ -n "$RPM_BUILD_ROOT" -a "$RPM_BUILD_ROOT" != / ] && rm -rf "$RPM_BUILD_ROOT"

%files
%defattr(-, root, root)
%doc README COPYING config.example keymap.example
%_bindir/*
%_datadir/%{name}/*
%_mandir/*/*
%dir %_libdir/moc/decoder_plugins

%if %{!?_without_musepack:1}0
%files musepack
%defattr(-, root, root)
%_libdir/moc/decoder_plugins/libmusepack_decoder.*
%endif

%if %{!?_without_flac:1}0
%files flac
%defattr(-, root, root)
%_libdir/moc/decoder_plugins/libflac_decoder.*
%endif

%if %{!?_without_mp3:1}0
%files mp3
%defattr(-, root, root)
%_libdir/moc/decoder_plugins/libmp3_decoder.*
%endif

%if %{!?_without_vorbis:1}0
%files vorbis
%defattr(-, root, root)
%_libdir/moc/decoder_plugins/libvorbis_decoder.*
%endif

%if %{!?_without_sndfile:1}0
%files sndfile
%defattr(-, root, root)
%_libdir/moc/decoder_plugins/libsndfile_formats_decoder.*
%endif

%if %{!?_without_speex:1}0
%files speex
%defattr(-, root, root)
%_libdir/moc/decoder_plugins/libspeex_decoder.*
%endif

%if %{!?_without_ffmpeg:1}0
%files ffmpeg
%defattr(-, root, root)
%_libdir/moc/decoder_plugins/libffmpeg_decoder.*
%endif

%changelog
* Thu Jul 26 2007 Klaus Ethgen <kethgen@inf.ethz.ch>
- Add changes from the daper 2.4.0 version
- Cleaning up
- Making the include/exclude mechanism unified (per default include all)
