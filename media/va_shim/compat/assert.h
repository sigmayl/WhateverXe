#pragma once
// The shim has no C++ runtime and cannot call __assert_fail. The builders use
// asserts as internal consistency checks; leave them out of the cross build.

#define assert(expression) ((void)0)
