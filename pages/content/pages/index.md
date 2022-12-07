title: libdatachannel
url:
save_as: index.html

<div id="home">
	<header>
		<p>libdatachannel is an open-source software library implementing WebRTC Data Channels, WebRTC Media Transport, and WebSockets. It is written in C++17 and offers C bindings. The <a href="https://github.com/paullouisageneau/libdatachannel">source code</a> is available under MPL 2.0, and the library is on <a href="https://aur.archlinux.org/packages/libdatachannel/">AUR</a>, <a href="https://github.com/Microsoft/vcpkg/tree/master/ports/libdatachannel">Vcpkg</a>, and <a href="https://github.com/Microsoft/vcpkg/tree/master/ports/libdatachannel">FreeBSD Ports</a>.</p>
		<div class="social">
			<a href="https://github.com/paullouisageneau/libdatachannel"><img src="/images/icon_github.png" alt="GitHub"></a>
			<a href="https://gitter.im/libdatachannel/community"><img src="/images/icon_gitter.png" alt="Gitter"></a>
			<a href="https://discord.gg/jXAP8jp3Nn"><img src="/images/icon_discord.png" alt="Discord"></a>
		</div>
	</header>
	<section>
		<img src="/images/icon_easy.png">
		<h3>Easy</h3>
		<ul>
			<li>Simple API inspired by the JavaScript API including WebSocket for signaling</li>
			<li>Minimal external dependencies (only <a href="https://www.openssl.org/">OpenSSL</a> or <a href="https://www.openssl.org/">GnuTLS</a>)
			<li>Lightweight and way easier to compile and use than Google's <a href="https://webrtc.googlesource.com/src/">reference library</a>
		</ul>
	</section>
	<section>
		<img src="/images/icon_compatible.png">
		<h3>Compatible</h3>
		<ul>
			<li>Compatible with browsers Firefox, Chromium, and Safari, and other WebRTC libraries (see <a href="https://github.com/sipsorcery/webrtc-echoes">webrtc-echoes</a>)</li>
			<li>Licensed under <a href="https://www.mozilla.org/en-US/MPL/2.0/FAQ/">MPL 2.0</a>, meaning software with any license may use the library</li>
			<li>Community-maintained bindings available for <a href="https://github.com/lerouxrgd/datachannel-rs">Rust</a>, <a href="https://github.com/murat-dogan/node-datachannel">Node.js</a>, and <a href="https://github.com/hanseuljun/datachannel-unity">Unity</a></li>
		</ul>
	</section>
	<section>
		<img src="/images/icon_portable.png">
		<h3>Portable</h3>
		<ul>
			<li>Support for POSIX platforms (including GNU/Linux, Android, FreeBSD, Apple macOS and iOS) and Microsoft Windows</li>
			<li>Support for both <a href="https://www.openssl.org/">OpenSSL</a> and <a href="https://www.gnutls.org/">GnuTLS</a> as TLS backend
			<li>Code using Data Channels and WebSockets may be compiled as-is to WebAssembly for browsers with <a href="https://github.com/paullouisageneau/datachannel-wasm">datachannel-wasm</a></li>
		</ul>
	</section>
	<div class="sponsor">
        <iframe src="https://github.com/sponsors/paullouisageneau/button" title="Sponsor paullouisageneau" height="35" width="116" style="border: 0;"></iframe>
        <div class="liberapay"><a href="https://liberapay.com/paullouisageneau/donate"><img alt="Donate using Liberapay" src="https://liberapay.com/assets/widgets/donate.svg"></a></div>
        <div class="ko-fi"><a href='https://ko-fi.com/A0A8CIDHU' target='_blank'><img height='36' style='border:0px;height:36px;' src='https://cdn.ko-fi.com/cdn/kofi3.png?v=3' border='0' alt='Buy Me a Coffee at ko-fi.com' /></a></div>
	</div>

