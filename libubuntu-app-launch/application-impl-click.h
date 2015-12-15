
#include <gio/gdesktopappinfo.h>
#include <json-glib/json-glib.h>
#include "application-impl-base.h"

#pragma once

namespace Ubuntu {
namespace AppLaunch {
namespace AppImpls {

class Click : public Base {
public:
	Click (const std::string &package,
	      const std::string &appname,
	      const std::string &version,
	      std::shared_ptr<Registry> registry);
	Click (const std::string &package,
	      const std::string &appname,
	      const std::string &version,
	      std::shared_ptr<JsonObject> manifest,
	      std::shared_ptr<Registry> registry);

	static std::list<std::shared_ptr<Application>> list (std::shared_ptr<Registry> registry);

	const std::string &package() override;
	const std::string &appname() override;
	const std::string &version() override;

	std::shared_ptr<Info> info() override;

private:
	std::string _package;
	std::string _appname;
	std::string _version;

	std::shared_ptr<JsonObject> _manifest;

	std::string _clickDir;
	std::shared_ptr<GDesktopAppInfo> _appinfo;

	static std::string manifestVersion (std::shared_ptr<JsonObject> manifest);
	static std::list<std::string> manifestApps (std::shared_ptr<JsonObject> manifest);
	static std::shared_ptr<GDesktopAppInfo> manifestAppDesktop (std::shared_ptr<JsonObject> manifest, const std::string &app, const std::string &clickDir);
};

}; // namespace AppImpls
}; // namespace AppLaunch
}; // namespace Ubuntu