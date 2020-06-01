/**
 * Copyright (c) 2019 Paul-Louis Ageneau
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#include <iostream>

using namespace std;

void test_connectivity();
void test_capi();
void test_websocket();

int main(int argc, char **argv) {
	try {
		cout << endl << "*** Running WebRTC connectivity test..." << endl;
		test_connectivity();
		cout << "*** Finished WebRTC connectivity test" << endl;
	} catch (const exception &e) {
		cerr << "WebRTC connectivity test failed: " << e.what() << endl;
		return -1;
	}
	try {
		cout << endl << "*** Running WebRTC C API test..." << endl;
		test_capi();
		cout << "*** Finished WebRTC C API test" << endl;
	} catch (const exception &e) {
		cerr << "WebRTC C API test failed: " << e.what() << endl;
		return -1;
	}
#if RTC_ENABLE_WEBSOCKET
	try {
		cout << endl << "*** Running WebSocket test..." << endl;
		test_websocket();
		cout << "*** Finished WebSocket test" << endl;
	} catch (const exception &e) {
		cerr << "WebSocket test failed: " << e.what() << endl;
		return -1;
	}
#endif
	return 0;
}
