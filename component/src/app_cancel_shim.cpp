// TEMPORARY shim, see app_cancel_shim.h. Dropped at merge in favour of the
// component_helpers.cpp implementation.

#include "stdafx.h"

#include "app_cancel_shim.h"

#include <SDK/initquit.h>

#include <atomic>

namespace flp {
namespace {

std::atomic<bool> g_quitting{ false };

class app_cancel_initquit : public initquit {
public:
	void on_quit() override { g_quitting = true; }
};

static initquit_factory_t<app_cancel_initquit> g_app_cancel_initquit_factory;

} // namespace

bool app_is_quitting() { return g_quitting.load() || fb2k::mainAborter().is_aborting(); }

cancel_token app_cancel_token() {
	cancel_token token;
	token.is_aborting = [] { return app_is_quitting(); };
	return token;
}

} // namespace flp
