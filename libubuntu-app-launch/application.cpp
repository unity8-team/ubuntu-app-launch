
extern "C" {
#include "app-info.h"
#include "ubuntu-app-launch.h"
}

#include "application.h"
#include "application-impl-click.h"
#include "application-impl-legacy.h"
#include "application-impl-libertine.h"

namespace Ubuntu {
namespace AppLaunch {

std::shared_ptr<Application>
Application::create (const std::string &package,
	             const std::string &appname,
	             const std::string &version,
	             std::shared_ptr<Registry> registry)
{
	auto appid = package + "_" + appname + "_" + version;
	if (app_info_legacy(appid.c_str(), NULL, NULL)) {
		return std::make_shared<AppImpls::Legacy>(package, registry);
	} else if (app_info_click(appid.c_str(), NULL, NULL)) {
		return std::make_shared<AppImpls::Click>(package, appname, version, registry);
	} else if (app_info_libertine(appid.c_str(), NULL, NULL)) {
		return std::make_shared<AppImpls::Libertine>(package, appname, registry);
	}
}

}; // namespace AppLaunch
}; // namespace Ubuntu