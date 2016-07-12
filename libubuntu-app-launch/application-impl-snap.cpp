/*
 * Copyright © 2016 Canonical Ltd.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 3, as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranties of
 * MERCHANTABILITY, SATISFACTORY QUALITY, or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Authors:
 *     Ted Gould <ted.gould@canonical.com>
 */

#include "application-impl-snap.h"
#include "application-info-desktop.h"
#include "registry-impl.h"

namespace ubuntu
{
namespace app_launch
{
namespace app_impls
{

/************************
 ** Interface Lists
 ************************/

/** All the interfaces that we support running applications with */
const std::set<std::string> SUPPORTED_INTERFACES{"unity8", "unity7", "x11"};
/** All the interfaces that we run XMir for by default */
const std::set<std::string> XMIR_INTERFACES{"unity7", "x11"};
/** All the interfaces that we tell Unity support lifecycle */
const std::set<std::string> LIFECYCLE_INTERFACES{"unity8"};

/************************
 ** Info support
 ************************/

class SnapInfo : public app_info::Desktop
{
    std::string interface_;

public:
    SnapInfo(const AppID& appid,
             const std::shared_ptr<Registry>& registry,
             const std::string& interface,
             const std::string& snapDir)
        : Desktop(
              [appid, snapDir]() -> std::shared_ptr<GKeyFile> {
                  std::string path = snapDir + "/meta/gui/" + appid.appname.value() + ".desktop";
                  std::shared_ptr<GKeyFile> keyfile(g_key_file_new(), g_key_file_free);
                  GError* error = nullptr;
                  g_key_file_load_from_file(keyfile.get(), path.c_str(), G_KEY_FILE_NONE, &error);
                  if (error != nullptr)
                  {
                      auto perror = std::shared_ptr<GError>(error, g_error_free);
                      throw std::runtime_error("Unable to find keyfile for '" + std::string(appid) + "' at '" + path +
                                               "' because: " + perror.get()->message);
                  }

                  return keyfile;
              }(),
              snapDir,
              registry,
              false)
        , interface_(interface)
    {
    }

    XMirEnable xMirEnable() override
    {
        if (XMIR_INTERFACES.find(interface_) != XMIR_INTERFACES.end())
        {
            return XMirEnable::from_raw(true);
        }
        else
        {
            return XMirEnable::from_raw(false);
        }
    }

    UbuntuLifecycle supportsUbuntuLifecycle() override
    {
        if (LIFECYCLE_INTERFACES.find(interface_) != LIFECYCLE_INTERFACES.end())
        {
            return UbuntuLifecycle::from_raw(true);
        }
        else
        {
            return UbuntuLifecycle::from_raw(false);
        }
    }
};

/************************
 ** Snap implementation
 ************************/

Snap::Snap(const AppID& appid, const std::shared_ptr<Registry>& registry, const std::string& interface)
    : Base(registry)
    , appid_(appid)
    , interface_(interface)
{
    pkgInfo_ = registry->impl->snapdInfo.pkgInfo(appid.package);
    if (!pkgInfo_)
    {
        throw std::runtime_error("Unable to get snap package info for AppID: " + std::string(appid));
    }

    if (pkgInfo_->revision != appid.version.value() ||
        pkgInfo_->appnames.find(appid.appname) == pkgInfo_->appnames.end())
    {
        throw std::runtime_error("AppID does not match installed package for: " + std::string(appid));
    }

    info_ = std::make_shared<SnapInfo>(appid_, _registry, interface_, pkgInfo_->directory);
}

Snap::Snap(const AppID& appid, const std::shared_ptr<Registry>& registry)
    : Snap(appid, registry, findInterface(appid, registry))
{
}

std::list<std::shared_ptr<Application>> Snap::list(const std::shared_ptr<Registry>& registry)
{
    std::list<std::shared_ptr<Application>> apps;

    for (auto interface : SUPPORTED_INTERFACES)
    {
        for (auto id : registry->impl->snapdInfo.appsForInterface(interface))
        {
            try
            {
                auto app = std::make_shared<Snap>(id, registry, interface);
                apps.push_back(app);
            }
            catch (std::runtime_error& e)
            {
                g_warning("Unable to make Snap object for '%s': %s", std::string(id).c_str(), e.what());
            }
        }
    }

    return apps;
}

AppID Snap::appId()
{
    return appid_;
}

std::string Snap::findInterface(const AppID& appid, const std::shared_ptr<Registry>& registry)
{
    auto ifaceset = registry->impl->snapdInfo.interfacesForAppId(appid);

    for (auto interface : SUPPORTED_INTERFACES)
    {
        if (ifaceset.find(interface) != ifaceset.end())
        {
            return interface;
        }
    }

    throw std::runtime_error("Interface not found for: " + std::string(appid));
}

bool Snap::hasAppId(const AppID& appId, const std::shared_ptr<Registry>& registry)
{
    try
    {
        auto pkginfo = registry->impl->snapdInfo.pkgInfo(appId.package);

        return pkginfo->revision == appId.version.value() &&
               pkginfo->appnames.find(appId.appname) != pkginfo->appnames.end();
    }
    catch (std::runtime_error& e)
    {
        return false;
    }
}

std::shared_ptr<Application::Info> Snap::info()
{
    return info_;
}

std::vector<std::shared_ptr<Application::Instance>> Snap::instances()
{
    std::vector<std::shared_ptr<Instance>> vect;
    auto startsWith = std::string(appId()) + "-";

    for (auto instance : _registry->impl->upstartInstancesForJob("application-snap"))
    {
        g_debug("Looking at snap instance: %s", instance.c_str());
        if (std::equal(startsWith.begin(), startsWith.end(), instance.begin()))
        {
            vect.emplace_back(std::make_shared<UpstartInstance>(appId(), "application-snap", instance, _registry));
        }
    }

    g_debug("Snap app '%s' has %d instances", std::string(appId()).c_str(), int(vect.size()));

    return vect;
}

std::list<std::pair<std::string, std::string>> Snap::launchEnv()
{
    g_debug("Getting snap specific environment");
    std::list<std::pair<std::string, std::string>> retval;

    retval.emplace_back(std::make_pair("APP_XMIR_ENABLE", info_->xMirEnable().value() ? "1" : "0"));
    retval.emplace_back(std::make_pair("APP_EXEC", info_->execLine().value()));

    return retval;
}

std::shared_ptr<Application::Instance> Snap::launch(const std::vector<Application::URL>& urls)
{
    g_debug("Launching a snap: %s", std::string(appId()).c_str());
    return UpstartInstance::launch(appId(), "application-snap", std::string(appId()) + "-", urls, _registry,
                                   UpstartInstance::launchMode::STANDARD, [this]() { return launchEnv(); });
}

std::shared_ptr<Application::Instance> Snap::launchTest(const std::vector<Application::URL>& urls)
{
    return UpstartInstance::launch(appId(), "application-snap", std::string(appId()) + "-", urls, _registry,
                                   UpstartInstance::launchMode::TEST, [this]() { return launchEnv(); });
}

}  // namespace app_impls
}  // namespace app_launch
}  // namespace ubuntu
