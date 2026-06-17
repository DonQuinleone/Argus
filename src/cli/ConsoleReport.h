#pragma once

#include "../core/Report.h"

namespace argus {

// Print a human-readable report to stdout (PASS/WARN/FAIL style, like adm_preflight).
void printReport(const Report& rep);

}  // namespace argus
