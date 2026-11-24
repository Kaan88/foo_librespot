// TEMPORARY shim: worker A adds these two functions to component_helpers.h
// (namespace flp). This header and app_cancel_shim.cpp are dropped at merge;
// the call sites then pick up the component_helpers.h declarations.

#pragma once

#include "bridge/pipe.h"

namespace flp {

// A cancel token that aborts once foobar2000 starts shutting down.
cancel_token app_cancel_token();

// True once foobar2000 has started shutting down.
bool app_is_quitting();

} // namespace flp
