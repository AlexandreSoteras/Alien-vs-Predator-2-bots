// compat/iostream.h - VC6's pre-standard <iostream.h>, which VS2022 no longer
// ships. The old headers put everything in the global namespace.
#pragma once
#include <iostream>
#include <istream>
#include <ostream>
#include <iomanip>
using namespace std;
