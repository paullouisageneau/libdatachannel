title: libdatachannel
url:
save_as: index.html

<div id="home">
	<p>
	libdatachannel is an open-source software library implementing WebRTC Data Channels, WebRTC Media Transport, and WebSockets. It is written in C++17 and offers C bindings. The <a href="https://github.com/paullouisageneau/libdatachannel">code source</a> is available under LGPLv2.
	</p>
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
			<li>Licensed under <a href="https://www.gnu.org/licenses/old-licenses/lgpl-2.1.en.html">LGPLv2</a>, meaning software with any license may link against the library</li>
			<li>Community-maintained bindings available for <a href="https://github.com/lerouxrgd/datachannel-rs">Rust</a>, <a href="https://github.com/murat-dogan/node-datachannel">Node.js</a>, and <a href="https://github.com/hanseuljun/datachannel-unity">Unity</a></li>
		</ul>
	</section>
	<section>
		<img src="/images/icon_portable.png">
		<h3>Portable</h3>
		<ul>
			<li>Support for POSIX platforms (including GNU/Linux, Android, Apple macOS, and FreeBSD) and Microsoft Windows</li>
			<li>Support for both <a href="https://www.openssl.org/">OpenSSL</a> and <a href="https://www.gnutls.org/">GnuTLS</a> as TLS backend
			<li>Code using Data Channels and WebSockets may be compiled as-is to WebAssembly for browsers with <a href="https://github.com/paullouisageneau/datachannel-wasm">datachannel-wasm</a></li>
		</ul>
	</section>
</div>

