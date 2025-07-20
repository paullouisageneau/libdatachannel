/**
 * Copyright (c) 2025 Michal Sledz
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

#include <functional>
#include <iostream>

using namespace std;

class TestResult {
public:
	bool success;
	string err_reason;

	TestResult(bool success, string err_reason = "") : success(success), err_reason(err_reason) {}
};

class Test {
public:
	string name;
	function<TestResult(void)> f;

	Test(string name, std::function<TestResult(void)> testFunc) : name(name), f(testFunc) {}

	TestResult run() {
		cout << endl << "*** Running " << name << " test" << endl;
		TestResult res = this->f();
		if (res.success) {
			cout << "*** Finished " << name << " test" << endl;
		} else {
			cerr << name << " test failed. Reason: " << res.err_reason << endl;
		}

		return res;
	}
};
