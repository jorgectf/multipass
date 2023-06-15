/*
 * Copyright (C) Canonical, Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "daemon.h"
#include "base_cloud_init_config.h"
#include "instance_settings_handler.h"

#include <multipass/alias_definition.h>
#include <multipass/constants.h>
#include <multipass/exceptions/blueprint_exceptions.h>
#include <multipass/exceptions/create_image_exception.h>
#include <multipass/exceptions/exitless_sshprocess_exception.h>
#include <multipass/exceptions/image_vault_exceptions.h>
#include <multipass/exceptions/invalid_memory_size_exception.h>
#include <multipass/exceptions/not_implemented_on_this_backend_exception.h>
#include <multipass/exceptions/snapshot_name_taken.h>
#include <multipass/exceptions/sshfs_missing_error.h>
#include <multipass/exceptions/start_exception.h>
#include <multipass/format.h>
#include <multipass/ip_address.h>
#include <multipass/json_utils.h>
#include <multipass/logging/client_logger.h>
#include <multipass/logging/log.h>
#include <multipass/name_generator.h>
#include <multipass/network_interface.h>
#include <multipass/platform.h>
#include <multipass/query.h>
#include <multipass/settings/settings.h>
#include <multipass/snapshot.h>
#include <multipass/ssh/ssh_session.h>
#include <multipass/sshfs_mount/sshfs_mount_handler.h>
#include <multipass/top_catch_all.h>
#include <multipass/utils.h>
#include <multipass/version.h>
#include <multipass/virtual_machine.h>
#include <multipass/virtual_machine_description.h>
#include <multipass/virtual_machine_factory.h>
#include <multipass/vm_image.h>
#include <multipass/vm_image_host.h>
#include <multipass/vm_image_vault.h>

#include <yaml-cpp/yaml.h>

#include <QDir>
#include <QEventLoop>
#include <QFutureSynchronizer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRegularExpression>
#include <QString>
#include <QSysInfo>
#include <QtConcurrent/QtConcurrent>

#include <algorithm>
#include <cassert>
#include <functional>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace mp = multipass;
namespace mpl = multipass::logging;
namespace mpu = multipass::utils;

namespace
{

using namespace std::chrono_literals;

using error_string = std::string;

constexpr auto category = "daemon";
constexpr auto instance_db_name = "multipassd-vm-instances.json";
constexpr auto reboot_cmd = "sudo reboot";
constexpr auto stop_ssh_cmd = "sudo systemctl stop ssh";
const std::string sshfs_error_template = "Error enabling mount support in '{}'"
                                         "\n\nPlease install the 'multipass-sshfs' snap manually inside the instance.";

// Images which cannot be bridged with --network.
const std::unordered_set<std::string> no_bridging_release = { // images to check from release and daily remotes
    "10.04",  "lucid", "11.10", "oneiric", "12.04",  "precise", "12.10",  "quantal", "13.04",
    "raring", "13.10", "saucy", "14.04",   "trusty", "14.10",   "utopic", "15.04",   "vivid",
    "15.10",  "wily",  "16.04", "xenial",  "16.10",  "yakkety", "17.04",  "zesty"};
const std::unordered_set<std::string> no_bridging_remote = {};                     // images with other remote specified
const std::unordered_set<std::string> no_bridging_remoteless = {"core", "core16"}; // images which do not use remote

mp::Query query_from(const mp::LaunchRequest* request, const std::string& name)
{
    if (!request->remote_name().empty() && request->image().empty())
        throw std::runtime_error("Must specify an image when specifying a remote");

    std::string image = request->image().empty() ? "default" : request->image();
    // TODO: persistence should be specified by the rpc as well

    mp::Query::Type query_type{mp::Query::Type::Alias};

    if (QString::fromStdString(image).startsWith("file"))
        query_type = mp::Query::Type::LocalFile;
    else if (QString::fromStdString(image).startsWith("http"))
        query_type = mp::Query::Type::HttpDownload;

    return {name, image, false, request->remote_name(), query_type, true};
}

auto make_cloud_init_vendor_config(const mp::SSHKeyProvider& key_provider, const std::string& username,
                                   const std::string& backend_version_string, const mp::CreateRequest* request)
{
    auto ssh_key_line = fmt::format("ssh-rsa {} {}@localhost", key_provider.public_key_as_base64(), username);
    QString pollinate_alias = QString::fromStdString(request->image());

    if (pollinate_alias.isEmpty())
    {
        pollinate_alias = "default";
    }
    else if (pollinate_alias.startsWith("http"))
    {
        pollinate_alias = "http";
    }
    else if (pollinate_alias.startsWith("file"))
    {
        pollinate_alias = "file";
    }

    auto remote_name = request->remote_name();
    auto config = YAML::Load(mp::base_cloud_init_config);
    config["ssh_authorized_keys"].push_back(ssh_key_line);
    config["timezone"] = request->time_zone();
    config["system_info"]["default_user"]["name"] = username;

    auto pollinate_user_agent_string =
        fmt::format("multipass/version/{} # written by Multipass\n", multipass::version_string);
    pollinate_user_agent_string += fmt::format("multipass/driver/{} # written by Multipass\n", backend_version_string);
    pollinate_user_agent_string +=
        fmt::format("multipass/host/{} # written by Multipass\n", multipass::platform::host_version());
    pollinate_user_agent_string += fmt::format("multipass/alias/{}{} # written by Multipass\n",
                                               !remote_name.empty() ? remote_name + ":" : "", pollinate_alias);

    YAML::Node pollinate_user_agent_node;
    pollinate_user_agent_node["path"] = "/etc/pollinate/add-user-agent";
    pollinate_user_agent_node["content"] = pollinate_user_agent_string;

    config["write_files"].push_back(pollinate_user_agent_node);

    return config;
}

auto make_cloud_init_meta_config(const std::string& name)
{
    YAML::Node meta_data;

    meta_data["instance-id"] = name;
    meta_data["local-hostname"] = name;
    meta_data["cloud-name"] = "multipass";

    return meta_data;
}

auto make_cloud_init_network_config(const std::string default_mac_addr,
                                    const std::vector<mp::NetworkInterface>& extra_interfaces)
{
    YAML::Node network_data;

    // Generate the cloud-init file only if there is at least one extra interface needing auto configuration.
    if (std::find_if(extra_interfaces.begin(), extra_interfaces.end(),
                     [](const auto& iface) { return iface.auto_mode; }) != extra_interfaces.end())
    {
        network_data["version"] = "2";

        std::string name = "default";
        network_data["ethernets"][name]["match"]["macaddress"] = default_mac_addr;
        network_data["ethernets"][name]["dhcp4"] = true;

        for (size_t i = 0; i < extra_interfaces.size(); ++i)
        {
            if (extra_interfaces[i].auto_mode)
            {
                name = "extra" + std::to_string(i);
                network_data["ethernets"][name]["match"]["macaddress"] = extra_interfaces[i].mac_address;
                network_data["ethernets"][name]["dhcp4"] = true;
                // We make the default gateway associated with the first interface.
                network_data["ethernets"][name]["dhcp4-overrides"]["route-metric"] = 200;
                // Make the interface optional, which means that networkd will not wait for the device to be configured.
                network_data["ethernets"][name]["optional"] = true;
            }
        }
    }

    return network_data;
}

void prepare_user_data(YAML::Node& user_data_config, YAML::Node& vendor_config)
{
    auto users = user_data_config["users"];
    if (users.IsSequence())
        users.push_back("default");

    auto keys = user_data_config["ssh_authorized_keys"];
    if (keys.IsSequence())
        keys.push_back(vendor_config["ssh_authorized_keys"][0]);
}

template <typename T>
auto name_from(const std::string& requested_name, const std::string& blueprint_name, mp::NameGenerator& name_gen,
               const T& currently_used_names)
{
    if (!requested_name.empty())
    {
        return requested_name;
    }
    else if (!blueprint_name.empty())
    {
        return blueprint_name;
    }
    else
    {
        auto name = name_gen.make_name();
        constexpr int num_retries = 100;
        for (int i = 0; i < num_retries; i++)
        {
            if (currently_used_names.find(name) != currently_used_names.end())
                continue;
            return name;
        }
        throw std::runtime_error("unable to generate a unique name");
    }
}

std::vector<mp::NetworkInterface> read_extra_interfaces(const QJsonObject& record)
{
    // Read the extra networks interfaces, if any.
    std::vector<mp::NetworkInterface> extra_interfaces;

    if (record.contains("extra_interfaces"))
    {
        for (QJsonValueRef entry : record["extra_interfaces"].toArray())
        {
            auto id = entry.toObject()["id"].toString().toStdString();
            auto mac_address = entry.toObject()["mac_address"].toString().toStdString();
            if (!mpu::valid_mac_address(mac_address))
            {
                throw std::runtime_error(fmt::format("Invalid MAC address {}", mac_address));
            }
            auto auto_mode = entry.toObject()["auto_mode"].toBool();
            extra_interfaces.push_back(mp::NetworkInterface{id, mac_address, auto_mode});
        }
    }

    return extra_interfaces;
}

std::unordered_map<std::string, mp::VMSpecs> load_db(const mp::Path& data_path, const mp::Path& cache_path)
{
    QDir data_dir{data_path};
    QDir cache_dir{cache_path};
    QFile db_file{data_dir.filePath(instance_db_name)};
    if (!db_file.open(QIODevice::ReadOnly))
    {
        // Try to open the old location
        db_file.setFileName(cache_dir.filePath(instance_db_name));
        if (!db_file.open(QIODevice::ReadOnly))
            return {};
    }

    QJsonParseError parse_error;
    auto doc = QJsonDocument::fromJson(db_file.readAll(), &parse_error);
    if (doc.isNull())
        return {};

    auto records = doc.object();
    if (records.isEmpty())
        return {};

    std::unordered_map<std::string, mp::VMSpecs> reconstructed_records;
    for (auto it = records.constBegin(); it != records.constEnd(); ++it)
    {
        auto key = it.key().toStdString();
        auto record = it.value().toObject();
        if (record.isEmpty())
            return {};

        auto num_cores = record["num_cores"].toInt();
        auto mem_size = record["mem_size"].toString().toStdString();
        auto disk_space = record["disk_space"].toString().toStdString();
        auto ssh_username = record["ssh_username"].toString().toStdString();
        auto state = record["state"].toInt();
        auto deleted = record["deleted"].toBool();
        auto metadata = record["metadata"].toObject();

        if (!num_cores && !deleted && ssh_username.empty() && metadata.isEmpty() &&
            !mp::MemorySize{mem_size}.in_bytes() && !mp::MemorySize{disk_space}.in_bytes())
        {
            mpl::log(mpl::Level::warning, category, fmt::format("Ignoring ghost instance in database: {}", key));
            continue;
        }

        if (ssh_username.empty())
            ssh_username = "ubuntu";

        // Read the default network interface, constructed from the "mac_addr" field.
        auto default_mac_address = record["mac_addr"].toString().toStdString();
        if (!mpu::valid_mac_address(default_mac_address))
        {
            throw std::runtime_error(fmt::format("Invalid MAC address {}", default_mac_address));
        }

        std::unordered_map<std::string, mp::VMMount> mounts;

        for (QJsonValueRef entry : record["mounts"].toArray())
        {
            mp::id_mappings uid_mappings;
            mp::id_mappings gid_mappings;

            auto target_path = entry.toObject()["target_path"].toString().toStdString();
            auto source_path = entry.toObject()["source_path"].toString().toStdString();

            for (QJsonValueRef uid_entry : entry.toObject()["uid_mappings"].toArray())
            {
                uid_mappings.push_back(
                    {uid_entry.toObject()["host_uid"].toInt(), uid_entry.toObject()["instance_uid"].toInt()});
            }

            for (QJsonValueRef gid_entry : entry.toObject()["gid_mappings"].toArray())
            {
                gid_mappings.push_back(
                    {gid_entry.toObject()["host_gid"].toInt(), gid_entry.toObject()["instance_gid"].toInt()});
            }

            uid_mappings = mp::unique_id_mappings(uid_mappings);
            gid_mappings = mp::unique_id_mappings(gid_mappings);
            auto mount_type = mp::VMMount::MountType(entry.toObject()["mount_type"].toInt());

            mp::VMMount mount{source_path, gid_mappings, uid_mappings, mount_type};
            mounts[target_path] = mount;
        }

        reconstructed_records[key] = {num_cores,
                                      mp::MemorySize{mem_size.empty() ? mp::default_memory_size : mem_size},
                                      mp::MemorySize{disk_space.empty() ? mp::default_disk_size : disk_space},
                                      default_mac_address,
                                      read_extra_interfaces(record),
                                      ssh_username,
                                      static_cast<mp::VirtualMachine::State>(state),
                                      mounts,
                                      deleted,
                                      metadata};
    }
    return reconstructed_records;
}

QJsonArray to_json_array(const std::vector<mp::NetworkInterface>& extra_interfaces)
{
    QJsonArray json;

    for (const auto& interface : extra_interfaces)
    {
        QJsonObject entry;
        entry.insert("id", QString::fromStdString(interface.id));
        entry.insert("mac_address", QString::fromStdString(interface.mac_address));
        entry.insert("auto_mode", interface.auto_mode);
        json.append(entry);
    }

    return json;
}

QJsonObject vm_spec_to_json(const mp::VMSpecs& specs)
{
    QJsonObject json;
    json.insert("num_cores", specs.num_cores);
    json.insert("mem_size", QString::number(specs.mem_size.in_bytes()));
    json.insert("disk_space", QString::number(specs.disk_space.in_bytes()));
    json.insert("ssh_username", QString::fromStdString(specs.ssh_username));
    json.insert("state", static_cast<int>(specs.state));
    json.insert("deleted", specs.deleted);
    json.insert("metadata", specs.metadata);

    // Write the networking information. Write first a field "mac_addr" containing the MAC address of the
    // default network interface. Then, write all the information about the rest of the interfaces.
    json.insert("mac_addr", QString::fromStdString(specs.default_mac_address));
    json.insert("extra_interfaces", to_json_array(specs.extra_interfaces));

    QJsonArray json_mounts;
    for (const auto& mount : specs.mounts)
    {
        QJsonObject entry;
        entry.insert("source_path", QString::fromStdString(mount.second.source_path));
        entry.insert("target_path", QString::fromStdString(mount.first));

        QJsonArray uid_mappings;

        for (const auto& map : mount.second.uid_mappings)
        {
            QJsonObject map_entry;
            map_entry.insert("host_uid", map.first);
            map_entry.insert("instance_uid", map.second);

            uid_mappings.append(map_entry);
        }

        entry.insert("uid_mappings", uid_mappings);

        QJsonArray gid_mappings;

        for (const auto& map : mount.second.gid_mappings)
        {
            QJsonObject map_entry;
            map_entry.insert("host_gid", map.first);
            map_entry.insert("instance_gid", map.second);

            gid_mappings.append(map_entry);
        }

        entry.insert("gid_mappings", gid_mappings);

        entry.insert("mount_type", static_cast<int>(mount.second.mount_type));
        json_mounts.append(entry);
    }

    json.insert("mounts", json_mounts);
    return json;
}

auto fetch_image_for(const std::string& name, const mp::FetchType& fetch_type, mp::VMImageVault& vault)
{
    auto stub_prepare = [](const mp::VMImage&) -> mp::VMImage { return {}; };
    auto stub_progress = [](int download_type, int progress) { return true; };

    mp::Query query{name, "", false, "", mp::Query::Type::Alias, false};

    return vault.fetch_image(fetch_type, query, stub_prepare, stub_progress, false, std::nullopt);
}

QDir instance_directory(const std::string& instance_name, const mp::DaemonConfig& config)
{ // TODO should we establish a more direct way to get to the instance's directory?
    return mp::utils::base_dir(fetch_image_for(instance_name, config.factory->fetch_type(), *config.vault).image_path);
}

auto try_mem_size(const std::string& val) -> std::optional<mp::MemorySize>
{
    try
    {
        return mp::MemorySize{val};
    }
    catch (mp::InvalidMemorySizeException& /*unused*/)
    {
        return std::nullopt;
    }
}

std::vector<mp::NetworkInterface> validate_extra_interfaces(const mp::LaunchRequest* request,
                                                            const mp::VirtualMachineFactory& factory,
                                                            std::vector<std::string>& nets_need_bridging,
                                                            mp::LaunchError& option_errors)
{
    std::vector<mp::NetworkInterface> interfaces;

    std::optional<std::vector<mp::NetworkInterfaceInfo>> factory_networks = std::nullopt;

    bool dont_allow_auto = false;
    std::string specified_image;

    auto remote = request->remote_name();
    auto image = request->image();

    if (request->remote_name().empty())
    {
        specified_image = image;

        dont_allow_auto = (no_bridging_remoteless.find(image) != no_bridging_remoteless.end()) ||
                          (no_bridging_release.find(image) != no_bridging_release.end());
    }
    else
    {
        specified_image = remote + ":" + image;

        dont_allow_auto = no_bridging_remote.find(specified_image) != no_bridging_remote.end();

        if (!dont_allow_auto && (remote == "release" || remote == "daily"))
            dont_allow_auto = no_bridging_release.find(image) != no_bridging_release.end();
    }

    for (const auto& net : request->network_options())
    {
        auto net_id = net.id();

        if (net_id == mp::bridged_network_name)
        {
            const auto bridged_id = MP_SETTINGS.get(mp::bridged_interface_key);
            if (bridged_id == "")
                throw std::runtime_error(
                    fmt::format("You have to `multipass set {}=<name>` to use the `--bridged` shortcut.",
                                mp::bridged_interface_key));
            net_id = bridged_id.toStdString();
        }

        if (!factory_networks)
        {
            try
            {
                factory_networks = factory.networks();
            }
            catch (const mp::NotImplementedOnThisBackendException&)
            {
                throw mp::NotImplementedOnThisBackendException("bridging");
            }
        }

        if (dont_allow_auto && net.mode() == multipass::LaunchRequest_NetworkOptions_Mode_AUTO)
        {
            throw std::runtime_error(fmt::format(
                "Automatic network configuration not available for {}. Consider using manual mode.", specified_image));
        }

        // Check that the id the user specified is valid.
        auto pred = [net_id](const mp::NetworkInterfaceInfo& info) { return info.id == net_id; };
        auto host_net_it = std::find_if(factory_networks->cbegin(), factory_networks->cend(), pred);

        if (host_net_it == factory_networks->cend())
        {
            if (net.id() == mp::bridged_network_name)
                throw std::runtime_error(
                    fmt::format("Invalid network '{}' set as bridged interface, use `multipass set {}=<name>` to "
                                "correct. See `multipass networks` for valid names.",
                                net_id, mp::bridged_interface_key));

            mpl::log(mpl::Level::warning, category, fmt::format("Invalid network name \"{}\"", net_id));
            option_errors.add_error_codes(mp::LaunchError::INVALID_NETWORK);
        }
        else if (host_net_it->needs_authorization)
            nets_need_bridging.push_back(host_net_it->id);

        // In case the user specified a MAC address, check it is valid.
        if (const auto& mac = QString::fromStdString(net.mac_address()).toLower().toStdString();
            mac.empty() || mpu::valid_mac_address(mac))
            interfaces.push_back(
                mp::NetworkInterface{net_id, mac, net.mode() != multipass::LaunchRequest_NetworkOptions_Mode_MANUAL});
        else
        {
            mpl::log(mpl::Level::warning, category, fmt::format("Invalid MAC address \"{}\"", mac));
            option_errors.add_error_codes(mp::LaunchError::INVALID_NETWORK);
        }
    }

    return interfaces;
}

void validate_image(const mp::LaunchRequest* request, const mp::VMImageVault& vault,
                    mp::VMBlueprintProvider& blueprint_provider)
{
    // TODO: Refactor this in such a way that we can use info returned here instead of ignoring it to avoid calls
    //       later that accomplish the same thing.
    try
    {
        if (!blueprint_provider.info_for(request->image()))
        {
            auto image_query = query_from(request, "");
            if (image_query.query_type == mp::Query::Type::Alias && vault.all_info_for(image_query).empty())
                throw mp::ImageNotFoundException(request->image(), request->remote_name());
        }
    }
    catch (const mp::IncompatibleBlueprintException&)
    {
        throw std::runtime_error(
            fmt::format("The \"{}\" Blueprint is not compatible with this host.", request->image()));
    }
}

auto validate_create_arguments(const mp::LaunchRequest* request, const mp::DaemonConfig* config)
{
    assert(config && config->factory && config->blueprint_provider && config->vault && "null ptr somewhere...");
    validate_image(request, *config->vault, *config->blueprint_provider);

    static const auto min_mem = try_mem_size(mp::min_memory_size);
    static const auto min_disk = try_mem_size(mp::min_disk_size);
    assert(min_mem && min_disk);

    auto mem_size_str = request->mem_size();
    auto disk_space_str = request->disk_space();
    auto instance_name = request->instance_name();
    auto option_errors = mp::LaunchError{};

    const auto opt_mem_size = try_mem_size(mem_size_str.empty() ? mp::default_memory_size : mem_size_str);

    mp::MemorySize mem_size{};
    if (opt_mem_size && *opt_mem_size >= min_mem)
        mem_size = *opt_mem_size;
    else
        option_errors.add_error_codes(mp::LaunchError::INVALID_MEM_SIZE);

    // If the user did not specify a disk size, then std::nullopt be passed down. Otherwise, the specified size will be
    // checked.
    std::optional<mp::MemorySize> disk_space{}; // std::nullopt by default.
    if (!disk_space_str.empty())
    {
        auto opt_disk_space = try_mem_size(disk_space_str);
        if (opt_disk_space && *opt_disk_space >= min_disk)
        {
            disk_space = opt_disk_space;
        }
        else
        {
            option_errors.add_error_codes(mp::LaunchError::INVALID_DISK_SIZE);
        }
    }

    if (!instance_name.empty() && !mp::utils::valid_hostname(instance_name))
        option_errors.add_error_codes(mp::LaunchError::INVALID_HOSTNAME);

    std::vector<std::string> nets_need_bridging;
    auto extra_interfaces = validate_extra_interfaces(request, *config->factory, nets_need_bridging, option_errors);

    struct CheckedArguments
    {
        mp::MemorySize mem_size;
        std::optional<mp::MemorySize> disk_space;
        std::string instance_name;
        std::vector<mp::NetworkInterface> extra_interfaces;
        std::vector<std::string> nets_need_bridging;
        mp::LaunchError option_errors;
    } ret{std::move(mem_size),         std::move(disk_space),         std::move(instance_name),
          std::move(extra_interfaces), std::move(nets_need_bridging), std::move(option_errors)};
    return ret;
}

auto connect_rpc(mp::DaemonRpc& rpc, mp::Daemon& daemon)
{
    QObject::connect(&rpc, &mp::DaemonRpc::on_create, &daemon, &mp::Daemon::create);
    QObject::connect(&rpc, &mp::DaemonRpc::on_launch, &daemon, &mp::Daemon::launch);
    QObject::connect(&rpc, &mp::DaemonRpc::on_purge, &daemon, &mp::Daemon::purge);
    QObject::connect(&rpc, &mp::DaemonRpc::on_find, &daemon, &mp::Daemon::find);
    QObject::connect(&rpc, &mp::DaemonRpc::on_info, &daemon, &mp::Daemon::info);
    QObject::connect(&rpc, &mp::DaemonRpc::on_list, &daemon, &mp::Daemon::list);
    QObject::connect(&rpc, &mp::DaemonRpc::on_networks, &daemon, &mp::Daemon::networks);
    QObject::connect(&rpc, &mp::DaemonRpc::on_mount, &daemon, &mp::Daemon::mount);
    QObject::connect(&rpc, &mp::DaemonRpc::on_recover, &daemon, &mp::Daemon::recover);
    QObject::connect(&rpc, &mp::DaemonRpc::on_ssh_info, &daemon, &mp::Daemon::ssh_info);
    QObject::connect(&rpc, &mp::DaemonRpc::on_start, &daemon, &mp::Daemon::start);
    QObject::connect(&rpc, &mp::DaemonRpc::on_stop, &daemon, &mp::Daemon::stop);
    QObject::connect(&rpc, &mp::DaemonRpc::on_suspend, &daemon, &mp::Daemon::suspend);
    QObject::connect(&rpc, &mp::DaemonRpc::on_restart, &daemon, &mp::Daemon::restart);
    QObject::connect(&rpc, &mp::DaemonRpc::on_delete, &daemon, &mp::Daemon::delet);
    QObject::connect(&rpc, &mp::DaemonRpc::on_umount, &daemon, &mp::Daemon::umount);
    QObject::connect(&rpc, &mp::DaemonRpc::on_version, &daemon, &mp::Daemon::version);
    QObject::connect(&rpc, &mp::DaemonRpc::on_get, &daemon, &mp::Daemon::get);
    QObject::connect(&rpc, &mp::DaemonRpc::on_set, &daemon, &mp::Daemon::set);
    QObject::connect(&rpc, &mp::DaemonRpc::on_keys, &daemon, &mp::Daemon::keys);
    QObject::connect(&rpc, &mp::DaemonRpc::on_authenticate, &daemon, &mp::Daemon::authenticate);
    QObject::connect(&rpc, &mp::DaemonRpc::on_snapshot, &daemon, &mp::Daemon::snapshot);
    QObject::connect(&rpc, &mp::DaemonRpc::on_restore, &daemon, &mp::Daemon::restore);
}

enum class InstanceGroup
{
    None,
    Operative,
    Deleted,
    All
};

using InstanceTable = std::unordered_map<std::string, mp::VirtualMachine::ShPtr>;
using InstanceTrail = std::variant<InstanceTable::iterator,                    // operative instances
                                   InstanceTable::iterator,                    // deleted instances
                                   std::reference_wrapper<const std::string>>; // missing instances

// careful to keep the original `name` around while the returned trail is in use!
InstanceTrail find_instance(InstanceTable& operative_instances, InstanceTable& deleted_instances,
                            const std::string& name)
{
    if (auto it = operative_instances.find(name); it != std::end(operative_instances))
        return InstanceTrail{std::in_place_index<0>, it};
    else if (it = deleted_instances.find(name); it != std::end(deleted_instances))
        return InstanceTrail{std::in_place_index<1>, it};
    else
        return {name};
}

using LinearInstanceSelection = std::vector<InstanceTable::iterator>;
using MissingInstanceList = std::vector<std::reference_wrapper<const std::string>>;
struct InstanceSelectionReport
{
    LinearInstanceSelection operative_selection;
    LinearInstanceSelection deleted_selection;
    MissingInstanceList missing_instances;
};

LinearInstanceSelection select_all(InstanceTable& instances)
{
    LinearInstanceSelection selection;
    selection.reserve(instances.size());

    for (auto it = instances.begin(); it != instances.end(); ++it)
        selection.push_back(it);

    return selection;
}

// careful to keep the original `name` around while the provided `selection` is in use!
void rank_instance(const std::string& name, const InstanceTrail& trail, InstanceSelectionReport& selection)
{
    switch (trail.index())
    {
    case 0:
        selection.operative_selection.push_back(std::get<0>(trail));
        break;
    case 1:
        selection.deleted_selection.push_back(std::get<1>(trail));
        break;
    case 2:
        selection.missing_instances.push_back(std::get<2>(trail));
        break;
    }
}

// careful to keep the original `names` around while the returned selection is in use!
template <typename InstanceNames>
InstanceSelectionReport select_instances(InstanceTable& operative_instances, InstanceTable& deleted_instances,
                                         const InstanceNames& names, InstanceGroup no_name_means)
{
    InstanceSelectionReport ret{};
    if (names.empty() && no_name_means != InstanceGroup::None)
    {
        if (no_name_means == InstanceGroup::Operative || no_name_means == InstanceGroup::All)
            ret.operative_selection = select_all(operative_instances);
        if (no_name_means == InstanceGroup::Deleted || no_name_means == InstanceGroup::All)
            ret.deleted_selection = select_all(deleted_instances);
    }
    else
    {
        std::unordered_set<std::string> seen_instances;

        for (const auto& name : names)
        {
            using T = std::decay_t<decltype(name)>;
            const std::string* vm_name;
            if constexpr (std::is_same_v<T, std::string>)
                vm_name = &name;
            else
                vm_name = &name.instance_name();

            if (seen_instances.insert(*vm_name).second)
            {
                auto trail = find_instance(operative_instances, deleted_instances, *vm_name);
                rank_instance(*vm_name, trail, ret);
            }
        }
    }

    return ret;
}

struct SelectionReaction
{
    struct ReactionComponent
    {
        grpc::StatusCode status_code;
        std::optional<std::string> message_template = std::nullopt;
    } operative_reaction, deleted_reaction, missing_reaction;
};

const SelectionReaction require_operative_instances_reaction{
    {grpc::StatusCode::OK},
    {grpc::StatusCode::INVALID_ARGUMENT, "instance \"{}\" is deleted"},
    {grpc::StatusCode::NOT_FOUND, "instance \"{}\" does not exist"}};

const SelectionReaction require_existing_instances_reaction{
    {grpc::StatusCode::OK}, // hands off clang-format
    {grpc::StatusCode::OK},
    {grpc::StatusCode::NOT_FOUND, "instance \"{}\" does not exist"}};

const SelectionReaction require_missing_instances_reaction{
    {grpc::StatusCode::INVALID_ARGUMENT, "instance \"{}\" already exists"},
    {grpc::StatusCode::INVALID_ARGUMENT, "instance \"{}\" already exists"},
    {grpc::StatusCode::OK}};

template <typename InstanceElem> // call only with InstanceTable::iterator or std::reference_wrapper<std::string>
const std::string& get_instance_name(InstanceElem instance_element)
{
    using T = std::decay_t<decltype(instance_element)>;

    if constexpr (std::is_same_v<T, LinearInstanceSelection::value_type>)
        return instance_element->first;
    else
    {
        static_assert(std::is_same_v<T, MissingInstanceList::value_type>);
        return instance_element.get();
    }
}

template <typename... Ts>
auto add_fmt_to(fmt::memory_buffer& buffer, Ts&&... fmt_params) -> std::back_insert_iterator<fmt::memory_buffer>
{
    if (buffer.size())
        buffer.push_back('\n');

    return fmt::format_to(std::back_inserter(buffer), std::forward<Ts>(fmt_params)...);
}

using SelectionComponent = std::variant<LinearInstanceSelection, MissingInstanceList>;
grpc::StatusCode react_to_component(const SelectionComponent& selection_component,
                                    const SelectionReaction::ReactionComponent& reaction_component,
                                    fmt::memory_buffer& errors)
{
    auto visitor = [&reaction_component, &errors](const auto& component) {
        auto status_code = grpc::StatusCode::OK;

        if (!component.empty())
        {
            const auto& msg_opt = reaction_component.message_template;
            status_code = reaction_component.status_code;

            if (msg_opt)
            {
                const auto& msg = *msg_opt;
                for (const auto& instance_element : component) // can be an iterator into an InstanceTable or a name
                {
                    const auto& instance_name = get_instance_name(instance_element);

                    if (status_code)
                        add_fmt_to(errors, msg, instance_name);
                    else
                        mpl::log(mpl::Level::debug, category, fmt::format(msg, instance_name));
                }
            }
        }

        return status_code;
    };

    return std::visit(visitor, selection_component);
}

auto grpc_status_for_mount_error(const std::string& instance_name)
{
    return grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, fmt::format(sshfs_error_template, instance_name));
}

auto grpc_status_for(fmt::memory_buffer& errors, grpc::StatusCode status_code = grpc::StatusCode::OK)
{
    if (errors.size() && !status_code)
        status_code = grpc::StatusCode::INVALID_ARGUMENT;

    return status_code ? grpc::Status(status_code,
                                      fmt::format("The following errors occurred:\n{}", fmt::to_string(errors)), "")
                       : grpc::Status::OK;
}

// Only the last bad status code is used
grpc::Status grpc_status_for_selection(const InstanceSelectionReport& selection, const SelectionReaction& reaction)
{
    fmt::memory_buffer errors;
    auto status_code = grpc::StatusCode::OK;

    if (auto code = react_to_component(selection.operative_selection, reaction.operative_reaction, errors); code)
        status_code = code;
    if (auto code = react_to_component(selection.deleted_selection, reaction.deleted_reaction, errors); code)
        status_code = code;
    if (auto code = react_to_component(selection.missing_instances, reaction.missing_reaction, errors); code)
        status_code = code;

    return grpc_status_for(errors, status_code);
}

grpc::Status grpc_status_for_instance_trail(const InstanceTrail& trail, const SelectionReaction& reaction)
{
    const std::string* instance_name = nullptr;
    const SelectionReaction::ReactionComponent* relevant_reaction_component = nullptr;

    switch (trail.index())
    {
    case 0:
        instance_name = &std::get<0>(trail)->first;
        relevant_reaction_component = &reaction.operative_reaction;
        break;
    case 1:
        instance_name = &std::get<1>(trail)->first;
        relevant_reaction_component = &reaction.deleted_reaction;
        break;
    case 2:
        instance_name = &std::get<2>(trail).get();
        relevant_reaction_component = &reaction.missing_reaction;
        break;
    default:
        assert(trail.index() && false && "shouldn't be here");
    }

    assert(relevant_reaction_component && instance_name);

    const auto& status_code = relevant_reaction_component->status_code;
    if (const auto& msg_opt = relevant_reaction_component->message_template; msg_opt)
    {
        const auto& msg = fmt::format(*msg_opt, *instance_name);
        if (status_code)
            return grpc::Status{status_code, msg, ""};

        mpl::log(mpl::Level::debug, category, msg);
    }

    return grpc::Status{status_code, "", ""};
}

std::pair<InstanceTrail, grpc::Status> find_instance_and_react(InstanceTable& operative_instances,
                                                               InstanceTable& deleted_instances,
                                                               const std::string& name,
                                                               const SelectionReaction& reaction)
{
    auto trail = find_instance(operative_instances, deleted_instances, name);
    auto status = grpc_status_for_instance_trail(trail, reaction);

    return {std::move(trail), status};
}

// careful to keep the original `names` around while the returned selection is in use!
template <typename InstanceNames>
std::pair<InstanceSelectionReport, grpc::Status>
select_instances_and_react(InstanceTable& operative_instances, InstanceTable& deleted_instances,
                           const InstanceNames& names, InstanceGroup no_name_means, const SelectionReaction& reaction)
{
    auto instance_selection = select_instances(operative_instances, deleted_instances, names, no_name_means);
    return {instance_selection, grpc_status_for_selection(instance_selection, reaction)};
}

std::string make_start_error_details(const InstanceSelectionReport& instance_selection)
{
    mp::StartError start_error;
    auto* errors = start_error.mutable_instance_errors();

    for (const auto& vm_it : instance_selection.deleted_selection)
        errors->insert({vm_it->first, mp::StartError::INSTANCE_DELETED});
    for (const auto& name : instance_selection.missing_instances)
        errors->insert({name, mp::StartError::DOES_NOT_EXIST});

    return start_error.SerializeAsString();
}

using VMCommand = std::function<grpc::Status(mp::VirtualMachine&)>;
grpc::Status cmd_vms(const LinearInstanceSelection& tgts, const VMCommand& cmd)
{
    // std::function involves some overhead, but it should be negligible here and
    // it gives clear error messages on type mismatch (!= templated callable).
    for (const auto& tgt : tgts)
    {
        auto vm_ptr = tgt->second;
        assert(vm_ptr && "no nulls please");

        if (auto st = cmd(*vm_ptr); !st.ok())
            return st; // Fail early
    }

    return grpc::Status::OK;
}

std::vector<std::string> names_from(const LinearInstanceSelection& instances)
{
    std::vector<std::string> ret;
    ret.reserve(instances.size());
    std::transform(std::cbegin(instances), std::cend(instances), std::back_inserter(ret),
                   [](const auto& item) { return item->first; });

    return ret;
}

template <typename Instances>
auto instances_running(const Instances& instances)
{
    for (const auto& instance : instances)
    {
        if (mp::utils::is_running(instance.second->current_state()))
            return true;
    }

    return false;
}

grpc::Status stop_accepting_ssh_connections(mp::SSHSession& session)
{
    auto proc = session.exec(stop_ssh_cmd);
    auto ecode = proc.exit_code();

    return ecode == 0 ? grpc::Status::OK
                      : grpc::Status{grpc::StatusCode::FAILED_PRECONDITION,
                                     fmt::format("Could not stop sshd. '{}' exited with code {}", stop_ssh_cmd, ecode),
                                     proc.read_std_error()};
}

grpc::Status ssh_reboot(const std::string& hostname, int port, const std::string& username,
                        const mp::SSHKeyProvider& key_provider)
{
    mp::SSHSession session{hostname, port, username, key_provider};

    // This allows us to later detect when the machine has finished restarting by waiting for SSH to be back up.
    // Otherwise, there would be a race condition, and we would be unable to distinguish whether it had ever been down.
    stop_accepting_ssh_connections(session);

    auto proc = session.exec(reboot_cmd);
    try
    {
        auto ecode = proc.exit_code();

        if (ecode != 0)
            return grpc::Status{grpc::StatusCode::FAILED_PRECONDITION,
                                fmt::format("Reboot command exited with code {}", ecode), proc.read_std_error()};
    }
    catch (const mp::ExitlessSSHProcessException&)
    {
        // this is the expected path
    }

    return grpc::Status::OK;
}

mp::InstanceStatus::Status grpc_instance_status_for(const mp::VirtualMachine::State& state)
{
    switch (state)
    {
    case mp::VirtualMachine::State::off:
    case mp::VirtualMachine::State::stopped:
        return mp::InstanceStatus::STOPPED;
    case mp::VirtualMachine::State::starting:
        return mp::InstanceStatus::STARTING;
    case mp::VirtualMachine::State::restarting:
        return mp::InstanceStatus::RESTARTING;
    case mp::VirtualMachine::State::running:
        return mp::InstanceStatus::RUNNING;
    case mp::VirtualMachine::State::delayed_shutdown:
        return mp::InstanceStatus::DELAYED_SHUTDOWN;
    case mp::VirtualMachine::State::suspending:
        return mp::InstanceStatus::SUSPENDING;
    case mp::VirtualMachine::State::suspended:
        return mp::InstanceStatus::SUSPENDED;
    case mp::VirtualMachine::State::unknown:
    default:
        return mp::InstanceStatus::UNKNOWN;
    }
}

// Computes the final size of an image, but also checks if the value given by the user is bigger than or equal than
// the size of the image.
mp::MemorySize compute_final_image_size(const mp::MemorySize image_size,
                                        std::optional<mp::MemorySize> command_line_value, mp::Path data_directory)
{
    mp::MemorySize disk_space{};

    if (!command_line_value)
    {
        auto default_disk_size_as_struct = mp::MemorySize(mp::default_disk_size);
        disk_space = image_size < default_disk_size_as_struct ? default_disk_size_as_struct : image_size;
    }
    else if (*command_line_value < image_size)
    {
        throw std::runtime_error(fmt::format("Requested disk ({} bytes) below minimum for this image ({} bytes)",
                                             command_line_value->in_bytes(), image_size.in_bytes()));
    }
    else
    {
        disk_space = *command_line_value;
    }

    auto available_bytes = MP_UTILS.filesystem_bytes_available(data_directory);
    if (available_bytes == -1)
    {
        throw std::runtime_error(fmt::format("Failed to determine information about the volume containing {}",
                                             data_directory.toStdString()));
    }
    std::string available_bytes_str = QString::number(available_bytes).toStdString();
    auto available_disk_space = mp::MemorySize(available_bytes_str + "B");

    if (available_disk_space < image_size)
    {
        throw std::runtime_error(fmt::format("Available disk ({} bytes) below minimum for this image ({} bytes)",
                                             available_disk_space.in_bytes(), image_size.in_bytes()));
    }

    if (available_disk_space < disk_space)
    {
        mpl::log(mpl::Level::warning, category,
                 fmt::format("Reserving more disk space ({} bytes) than available ({} bytes)", disk_space.in_bytes(),
                             available_disk_space.in_bytes()));
    }

    return disk_space;
}

std::unordered_set<std::string> mac_set_from(const mp::VMSpecs& spec)
{
    std::unordered_set<std::string> macs{};

    macs.insert(spec.default_mac_address);

    for (const auto& extra_iface : spec.extra_interfaces)
        macs.insert(extra_iface.mac_address);

    return macs;
}

// Merge the contents of t into s, iff the sets are disjoint (i.e. make s = sUt). Return whether s and t were disjoint.
bool merge_if_disjoint(std::unordered_set<std::string>& s, const std::unordered_set<std::string>& t)
{
    if (any_of(cbegin(s), cend(s), [&t](const auto& mac) { return t.find(mac) != cend(t); }))
        return false;

    s.insert(cbegin(t), cend(t));
    return true;
}

// Generate a MAC address which does not exist in the set s. Then add the address to s.
std::string generate_unused_mac_address(std::unordered_set<std::string>& s)
{
    // TODO: Checking in our list of MAC addresses does not suffice to conclude the generated MAC is unique. We
    // should also check in the ARP table.
    static constexpr auto max_tries = 5;
    for (auto i = 0; i < max_tries; ++i)
        if (auto [it, success] = s.insert(mp::utils::generate_mac_address()); success)
            return *it;

    throw std::runtime_error{
        fmt::format("Failed to generate an unique mac address after {} attempts. Number of mac addresses in use: {}",
                    max_tries, s.size())};
}

bool is_ipv4_valid(const std::string& ipv4)
{
    try
    {
        (mp::IPAddress(ipv4));
    }
    catch (std::invalid_argument&)
    {
        return false;
    }

    return true;
}

using InstanceSnapshotPairs = google::protobuf::RepeatedPtrField<mp::InstanceSnapshotPair>;
std::unordered_map<std::string, std::unordered_set<std::string>>
map_snapshots_to_instances(const InstanceSnapshotPairs& instances_snapshots)
{
    std::unordered_map<std::string, std::unordered_set<std::string>> instance_snapshots_map;

    for (const auto& it : instances_snapshots)
    {
        const auto& instance = it.instance_name();
        const auto& snapshot = it.snapshot_name();

        if (snapshot.empty())
            instance_snapshots_map[instance].clear();
        else if (const auto& entry = instance_snapshots_map.find(instance);
                 entry == instance_snapshots_map.end() || !entry->second.empty())
            instance_snapshots_map[instance].insert(snapshot);
    }

    return instance_snapshots_map;
}

void add_aliases(google::protobuf::RepeatedPtrField<mp::FindReply_ImageInfo>* container, const std::string& remote_name,
                 const mp::VMImageInfo& info, const std::string& default_remote)
{
    if (!info.aliases.empty())
    {
        auto entry = container->Add();
        for (const auto& alias : info.aliases)
        {
            auto alias_entry = entry->add_aliases_info();
            if (remote_name != default_remote)
            {
                alias_entry->set_remote_name(remote_name);
            }
            alias_entry->set_alias(alias.toStdString());
        }

        entry->set_os(info.os.toStdString());
        entry->set_release(info.release_title.toStdString());
        entry->set_version(info.version.toStdString());
    }
}

auto timeout_for(const int requested_timeout, const int blueprint_timeout)
{
    if (requested_timeout > 0)
        return std::chrono::seconds(requested_timeout);

    if (blueprint_timeout > 0)
        return std::chrono::seconds(blueprint_timeout);

    return mp::default_timeout;
}

mp::SettingsHandler*
register_instance_mod(std::unordered_map<std::string, mp::VMSpecs>& vm_instance_specs,
                      std::unordered_map<std::string, mp::VirtualMachine::ShPtr>& vm_instances,
                      const std::unordered_map<std::string, mp::VirtualMachine::ShPtr>& deleted_instances,
                      const std::unordered_set<std::string>& preparing_instances,
                      std::function<void()> instance_persister)
{
    return MP_SETTINGS.register_handler(std::make_unique<mp::InstanceSettingsHandler>(
        vm_instance_specs, vm_instances, deleted_instances, preparing_instances, std::move(instance_persister)));
}

} // namespace

mp::Daemon::Daemon(std::unique_ptr<const DaemonConfig> the_config)
    : config{std::move(the_config)},
      vm_instance_specs{load_db(
          mp::utils::backend_directory_path(config->data_directory, config->factory->get_backend_directory_name()),
          mp::utils::backend_directory_path(config->cache_directory, config->factory->get_backend_directory_name()))},
      daemon_rpc{config->server_address, *config->cert_provider, config->client_cert_store.get()},
      instance_mod_handler{register_instance_mod(vm_instance_specs, operative_instances, deleted_instances,
                                                 preparing_instances, [this] { persist_instances(); })}
{
    connect_rpc(daemon_rpc, *this);
    std::vector<std::string> invalid_specs;

    try
    {
        config->factory->hypervisor_health_check();
    }
    catch (const std::runtime_error& e)
    {
        mpl::log(mpl::Level::warning, category, fmt::format("Hypervisor health check failed: {}", e.what()));
    }

    for (auto& entry : vm_instance_specs)
    {
        const auto& name = entry.first;
        auto& spec = entry.second;

        if (!config->vault->has_record_for(name))
        {
            invalid_specs.push_back(name);
            continue;
        }

        // Check that all the interfaces in the instance have different MAC address, and that they were not used in
        // the other instances. String validity was already checked in load_db(). Add these MAC's to the daemon's set
        // only if this instance is not invalid.
        auto new_macs = mac_set_from(spec);

        if (new_macs.size() <= spec.extra_interfaces.size() || !merge_if_disjoint(new_macs, allocated_mac_addrs))
        {
            // There is at least one repeated address in new_macs.
            mpl::log(mpl::Level::warning, category, fmt::format("{} has repeated MAC addresses", name));
            invalid_specs.push_back(name);
            continue;
        }

        auto vm_image = fetch_image_for(name, config->factory->fetch_type(), *config->vault);
        if (!vm_image.image_path.isEmpty() && !QFile::exists(vm_image.image_path))
        {
            mpl::log(mpl::Level::warning, category,
                     fmt::format("Could not find image for '{}'. Expected location: {}", name, vm_image.image_path));
            invalid_specs.push_back(name);
            continue;
        }

        const auto instance_dir = mp::utils::base_dir(vm_image.image_path);
        const auto cloud_init_iso = instance_dir.filePath("cloud-init-config.iso");
        mp::VirtualMachineDescription vm_desc{spec.num_cores,
                                              spec.mem_size,
                                              spec.disk_space,
                                              name,
                                              spec.default_mac_address,
                                              spec.extra_interfaces,
                                              spec.ssh_username,
                                              vm_image,
                                              cloud_init_iso,
                                              {},
                                              {},
                                              {},
                                              {}};

        auto& instance_record = spec.deleted ? deleted_instances : operative_instances;
        auto instance = instance_record[name] = config->factory->create_virtual_machine(vm_desc, *this);
        instance->load_snapshots(instance_directory(name, *config));

        allocated_mac_addrs = std::move(new_macs); // Add the new macs to the daemon's list only if we got this far

        // FIXME: somehow we're writing contradictory state to disk.
        if (spec.deleted && spec.state != VirtualMachine::State::stopped)
        {
            mpl::log(mpl::Level::warning, category,
                     fmt::format("{} is deleted but has incompatible state {}, resetting state to 0 (stopped)", name,
                                 static_cast<int>(spec.state)));
            spec.state = VirtualMachine::State::stopped;
        }

        if (!spec.deleted)
            init_mounts(name);
        std::unique_lock lock{start_mutex};
        if (spec.state == VirtualMachine::State::running &&
            operative_instances[name]->current_state() != VirtualMachine::State::running &&
            operative_instances[name]->current_state() != VirtualMachine::State::starting)
        {
            assert(!spec.deleted);
            mpl::log(mpl::Level::info, category, fmt::format("{} needs starting. Starting now...", name));

            multipass::top_catch_all(name, [this, &name, &lock]() {
                operative_instances[name]->start();
                lock.unlock();
                on_restart(name);
            });
        }
    }

    for (const auto& bad_spec : invalid_specs)
    {
        mpl::log(mpl::Level::warning, category, fmt::format("Removing invalid instance: {}", bad_spec));
        vm_instance_specs.erase(bad_spec);
    }

    if (!invalid_specs.empty())
        persist_instances();

    config->vault->prune_expired_images();

    // Fire timer every six hours to perform maintenance on source images such as
    // pruning expired images and updating to newly released images.
    connect(&source_images_maintenance_task, &QTimer::timeout, [this]() {
        if (image_update_future.isRunning())
        {
            mpl::log(mpl::Level::info, category, "Image updater already running. Skipping…");
        }
        else
        {
            image_update_future = QtConcurrent::run([this] {
                config->vault->prune_expired_images();

                auto prepare_action = [this](const VMImage& source_image) -> VMImage {
                    return config->factory->prepare_source_image(source_image);
                };

                auto download_monitor = [](int download_type, int percentage) {
                    static int last_percentage_logged = -1;
                    if (percentage % 10 == 0)
                    {
                        // Note: The progress callback may be called repeatedly with the same percentage,
                        // so this logic is to only log it once
                        if (last_percentage_logged != percentage)
                        {
                            mpl::log(mpl::Level::info, category, fmt::format("  {}%", percentage));
                            last_percentage_logged = percentage;
                        }
                    }
                    return true;
                };

                try
                {
                    config->vault->update_images(config->factory->fetch_type(), prepare_action, download_monitor);
                }
                catch (const std::exception& e)
                {
                    mpl::log(mpl::Level::error, category, fmt::format("Error updating images: {}", e.what()));
                }
            });
        }
    });
    source_images_maintenance_task.start(config->image_refresh_timer);
}

mp::Daemon::~Daemon()
{
    mp::top_catch_all(category, [this] { MP_SETTINGS.unregister_handler(instance_mod_handler); });
}

void mp::Daemon::create(const CreateRequest* request,
                        grpc::ServerReaderWriterInterface<CreateReply, CreateRequest>* server,
                        std::promise<grpc::Status>* status_promise) // clang-format off
try // clang-format on
{
    mpl::ClientLogger<CreateReply, CreateRequest> logger{mpl::level_from(request->verbosity_level()), *config->logger,
                                                         server};
    return create_vm(request, server, status_promise, /*start=*/false);
}
catch (const std::exception& e)
{
    status_promise->set_value(grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, e.what(), ""));
}

void mp::Daemon::launch(const LaunchRequest* request,
                        grpc::ServerReaderWriterInterface<LaunchReply, LaunchRequest>* server,
                        std::promise<grpc::Status>* status_promise) // clang-format off
try // clang-format on
{
    mpl::ClientLogger<LaunchReply, LaunchRequest> logger{mpl::level_from(request->verbosity_level()), *config->logger,
                                                         server};

    return create_vm(request, server, status_promise, /*start=*/true);
}
catch (const mp::StartException& e)
{
    auto name = e.name();

    release_resources(name);
    operative_instances.erase(name);
    persist_instances();

    status_promise->set_value(grpc::Status(grpc::StatusCode::ABORTED, e.what(), ""));
}
catch (const std::exception& e)
{
    status_promise->set_value(grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, e.what(), ""));
}

void mp::Daemon::purge(const PurgeRequest* request, grpc::ServerReaderWriterInterface<PurgeReply, PurgeRequest>* server,
                       std::promise<grpc::Status>* status_promise) // clang-format off
try // clang-format on
{
    PurgeReply response;

    for (const auto& del : deleted_instances)
    {
        release_resources(del.first);
        response.add_purged_instances(del.first);
    }

    deleted_instances.clear();
    persist_instances();

    server->Write(response);
    status_promise->set_value(grpc::Status::OK);
}
catch (const std::exception& e)
{
    status_promise->set_value(grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, e.what(), ""));
}

void mp::Daemon::find(const FindRequest* request, grpc::ServerReaderWriterInterface<FindReply, FindRequest>* server,
                      std::promise<grpc::Status>* status_promise) // clang-format off
try // clang-format on
{
    mpl::ClientLogger<FindReply, FindRequest> logger{mpl::level_from(request->verbosity_level()), *config->logger,
                                                     server};
    FindReply response;
    response.set_show_images(request->show_images());
    response.set_show_blueprints(request->show_blueprints());

    const auto default_remote{"release"};

    if (!request->search_string().empty())
    {
        if (!request->remote_name().empty())
        {
            // This is a compromised solution for now, it throws if remote_name is invalid.
            // In principle, it should catch the returned VMImageHost in the valid remote_name case and
            // get the found VMImageHost reused in the follow-up code. However, because of the current framework,
            // That would involve more changes because the query carries the remote name and there is
            // another dispatch in the all_info_for function.
            const auto& remote_name = request->remote_name();
            config->vault->image_host_for(remote_name);
        }

        if (request->show_images())
        {
            std::vector<std::pair<std::string, VMImageInfo>> vm_images_info;

            try
            {
                vm_images_info =
                    config->vault->all_info_for({"", request->search_string(), false, request->remote_name(),
                                                 Query::Type::Alias, request->allow_unsupported()});
            }
            catch (const std::exception& e)
            {
                mpl::log(mpl::Level::warning, category,
                         fmt::format("An unexpected error occurred while fetching images matching \"{}\": {}",
                                     request->search_string(), e.what()));
            }

            for (auto& [remote, info] : vm_images_info)
            {
                if (info.aliases.contains(QString::fromStdString(request->search_string())))
                    info.aliases = QStringList({QString::fromStdString(request->search_string())});
                else
                    info.aliases = QStringList({info.id.left(12)});

                auto remote_name =
                    (!request->remote_name().empty() ||
                     (request->remote_name().empty() && vm_images_info.size() > 1 && remote != default_remote))
                        ? remote
                        : "";

                add_aliases(response.mutable_images_info(), remote_name, info, "");
            }
        }

        if (request->show_blueprints())
        {
            std::optional<VMImageInfo> info;
            try
            {
                info = config->blueprint_provider->info_for(request->search_string());
            }
            catch (const std::exception& e)
            {
                mpl::log(mpl::Level::warning, category,
                         fmt::format("An unexpected error occurred while fetching blueprints matching \"{}\": {}",
                                     request->search_string(), e.what()));
            }

            if (info)
            {
                if ((*info).aliases.contains(QString::fromStdString(request->search_string())))
                    (*info).aliases = QStringList({QString::fromStdString(request->search_string())});
                else
                    (*info).aliases = QStringList({(*info).id.left(12)});

                add_aliases(response.mutable_blueprints_info(), "", *info, "");
            }
        }
    }
    else if (request->remote_name().empty())
    {
        if (request->show_images())
        {
            for (const auto& image_host : config->image_hosts)
            {
                std::unordered_set<std::string> images_found;
                auto action = [&images_found, &default_remote, request, &response](const std::string& remote,
                                                                                   const mp::VMImageInfo& info) {
                    if ((info.supported || request->allow_unsupported()) && !info.aliases.empty() &&
                        images_found.find(info.release_title.toStdString()) == images_found.end())
                    {
                        add_aliases(response.mutable_images_info(), remote, info, default_remote);
                        images_found.insert(info.release_title.toStdString());
                    }
                };

                image_host->for_each_entry_do(action);
            }
        }

        if (request->show_blueprints())
        {
            auto vm_blueprints_info = config->blueprint_provider->all_blueprints();

            for (const auto& info : vm_blueprints_info)
                add_aliases(response.mutable_blueprints_info(), "", info, "");
        }
    }
    else
    {
        const auto& remote = request->remote_name();
        auto image_host = config->vault->image_host_for(remote);
        auto vm_images_info = image_host->all_images_for(remote, request->allow_unsupported());

        for (const auto& info : vm_images_info)
            add_aliases(response.mutable_images_info(), remote, info, "");
    }

    server->Write(response);
    status_promise->set_value(grpc::Status::OK);
}
catch (const std::exception& e)
{
    status_promise->set_value(grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, e.what(), ""));
}

void mp::Daemon::info(const InfoRequest* request, grpc::ServerReaderWriterInterface<InfoReply, InfoRequest>* server,
                      std::promise<grpc::Status>* status_promise) // clang-format off
try // clang-format on
{
    mpl::ClientLogger<InfoReply, InfoRequest> logger{mpl::level_from(request->verbosity_level()), *config->logger,
                                                     server};
    InfoReply response;

    // Need to 'touch' a report in the response so formatters know what to do with an otherwise empty response
    request->snapshot_overview() ? (void)response.mutable_snapshot_overview()
                                 : (void)response.mutable_detailed_report();
    bool have_mounts = false;
    bool deleted = false;
    auto fetch_instance_info = [&](VirtualMachine& vm) {
        const auto& name = vm.vm_name;
        auto info = response.mutable_detailed_report()->add_details();
        auto instance_info = info->mutable_instance_info();
        auto present_state = vm.current_state();
        info->set_name(name);
        if (deleted)
        {
            info->mutable_instance_status()->set_status(mp::InstanceStatus::DELETED);
        }
        else
        {
            info->mutable_instance_status()->set_status(grpc_instance_status_for(present_state));
        }

        auto vm_image = fetch_image_for(name, config->factory->fetch_type(), *config->vault);
        auto original_release = vm_image.original_release;

        if (!vm_image.id.empty() && original_release.empty())
        {
            try
            {
                auto vm_image_info = config->image_hosts.back()->info_for_full_hash(vm_image.id);
                original_release = vm_image_info.release_title.toStdString();
            }
            catch (const std::exception& e)
            {
                mpl::log(mpl::Level::warning, category, fmt::format("Cannot fetch image information: {}", e.what()));
            }
        }

        instance_info->set_num_snapshots(vm.get_num_snapshots());
        instance_info->set_image_release(original_release);
        instance_info->set_id(vm_image.id);

        auto vm_specs = vm_instance_specs[name];

        auto mount_info = info->mutable_mount_info();

        mount_info->set_longest_path_len(0);

        if (!vm_specs.mounts.empty())
            have_mounts = true;

        if (MP_SETTINGS.get_as<bool>(mp::mounts_key))
        {
            for (const auto& mount : vm_specs.mounts)
            {
                if (mount.second.source_path.size() > mount_info->longest_path_len())
                {
                    mount_info->set_longest_path_len(mount.second.source_path.size());
                }

                auto entry = mount_info->add_mount_paths();
                entry->set_source_path(mount.second.source_path);
                entry->set_target_path(mount.first);

                for (const auto& uid_mapping : mount.second.uid_mappings)
                {
                    auto uid_pair = entry->mutable_mount_maps()->add_uid_mappings();
                    uid_pair->set_host_id(uid_mapping.first);
                    uid_pair->set_instance_id(uid_mapping.second);
                }
                for (const auto& gid_mapping : mount.second.gid_mappings)
                {
                    auto gid_pair = entry->mutable_mount_maps()->add_gid_mappings();
                    gid_pair->set_host_id(gid_mapping.first);
                    gid_pair->set_instance_id(gid_mapping.second);
                }
            }
        }

        if (!request->no_runtime_information() && mp::utils::is_running(present_state))
        {
            mp::SSHSession session{vm.ssh_hostname(), vm.ssh_port(), vm_specs.ssh_username, *config->ssh_key_provider};

            instance_info->set_load(mpu::run_in_ssh_session(session, "cat /proc/loadavg | cut -d ' ' -f1-3"));
            instance_info->set_memory_usage(
                mpu::run_in_ssh_session(session, "free -b | grep 'Mem:' | awk '{printf $3}'"));
            info->set_memory_total(mpu::run_in_ssh_session(session, "free -b | grep 'Mem:' | awk '{printf $2}'"));
            instance_info->set_disk_usage(
                mpu::run_in_ssh_session(session, "df -t ext4 -t vfat --total -B1 --output=used | tail -n 1"));
            info->set_disk_total(
                mpu::run_in_ssh_session(session, "df -t ext4 -t vfat --total -B1 --output=size | tail -n 1"));
            info->set_cpu_count(mpu::run_in_ssh_session(session, "nproc"));

            std::string management_ip = vm.management_ipv4();
            auto all_ipv4 = vm.get_all_ipv4(*config->ssh_key_provider);

            if (is_ipv4_valid(management_ip))
                instance_info->add_ipv4(management_ip);
            else if (all_ipv4.empty())
                instance_info->add_ipv4("N/A");

            for (const auto& extra_ipv4 : all_ipv4)
                if (extra_ipv4 != management_ip)
                    instance_info->add_ipv4(extra_ipv4);

            auto current_release =
                mpu::run_in_ssh_session(session, "cat /etc/os-release | grep 'PRETTY_NAME' | cut -d \\\" -f2");
            instance_info->set_current_release(!current_release.empty() ? current_release : original_release);
        }
        return grpc::Status::OK;
    };

    std::unordered_map<std::string, std::unordered_set<std::string>> instance_snapshots_map;
    auto fetch_snapshot_overview = [&](VirtualMachine& vm) {
        fmt::memory_buffer errors;
        const auto& name = vm.vm_name;

        auto get_snapshot_info = [&](std::shared_ptr<const Snapshot> snapshot) {
            auto overview = response.mutable_snapshot_overview()->add_overview();
            auto fundamentals = overview->mutable_fundamentals();

            overview->set_instance_name(name);
            fundamentals->set_snapshot_name(snapshot->get_name());
            fundamentals->set_parent(snapshot->get_parent_name());
            fundamentals->set_comment(snapshot->get_comment());
            // TODO@snapshots populate snapshot creation time once available
        };

        if (const auto& it = instance_snapshots_map.find(name);
            it == instance_snapshots_map.end() || it->second.empty())
        {
            for (const auto& snapshot : vm.view_snapshots())
                get_snapshot_info(snapshot);
        }
        else
        {
            for (const auto& snapshot : it->second)
            {
                try
                {
                    get_snapshot_info(vm.get_snapshot(snapshot));
                }
                catch (const std::out_of_range&)
                {
                    add_fmt_to(errors, "snapshot \"{}\" does not exist", snapshot);
                }
            }
        }

        return grpc_status_for(errors);
    };

    auto [instance_selection, status] =
        select_instances_and_react(operative_instances, deleted_instances, request->instances_snapshots(),
                                   InstanceGroup::All, require_existing_instances_reaction);

    if (status.ok())
    {
        instance_snapshots_map = map_snapshots_to_instances(request->instances_snapshots());

        // TODO@snapshots change cmd logic to include detailed snapshot info output
        auto cmd =
            request->snapshot_overview() ? std::function(fetch_snapshot_overview) : std::function(fetch_instance_info);

        if ((status = cmd_vms(instance_selection.operative_selection, cmd)).ok())
        {
            deleted = true;
            status = cmd_vms(instance_selection.deleted_selection, cmd);
        }

        if (have_mounts && !MP_SETTINGS.get_as<bool>(mp::mounts_key))
            mpl::log(mpl::Level::error, category, "Mounts have been disabled on this instance of Multipass");

        server->Write(response);
    }

    status_promise->set_value(status);
}
catch (const std::exception& e)
{
    status_promise->set_value(grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, e.what(), ""));
}

void mp::Daemon::list(const ListRequest* request, grpc::ServerReaderWriterInterface<ListReply, ListRequest>* server,
                      std::promise<grpc::Status>* status_promise) // clang-format off
try // clang-format on
{
    mpl::ClientLogger<ListReply, ListRequest> logger{mpl::level_from(request->verbosity_level()), *config->logger,
                                                     server};
    ListReply response;
    config->update_prompt->populate_if_time_to_show(response.mutable_update_info());

    for (const auto& instance : operative_instances)
    {
        const auto& name = instance.first;
        const auto& vm = instance.second;
        auto present_state = vm->current_state();
        auto entry = response.add_instances();
        entry->set_name(name);
        entry->mutable_instance_status()->set_status(grpc_instance_status_for(present_state));

        // FIXME: Set the release to the cached current version when supported
        auto vm_image = fetch_image_for(name, config->factory->fetch_type(), *config->vault);
        auto current_release = vm_image.original_release;

        if (!vm_image.id.empty() && current_release.empty())
        {
            try
            {
                auto vm_image_info = config->image_hosts.back()->info_for_full_hash(vm_image.id);
                current_release = vm_image_info.release_title.toStdString();
            }
            catch (const std::exception& e)
            {
                mpl::log(mpl::Level::warning, category, fmt::format("Cannot fetch image information: {}", e.what()));
            }
        }

        entry->set_current_release(current_release);

        if (request->request_ipv4() && mp::utils::is_running(present_state))
        {
            std::string management_ip = vm->management_ipv4();
            auto all_ipv4 = vm->get_all_ipv4(*config->ssh_key_provider);

            if (is_ipv4_valid(management_ip))
                entry->add_ipv4(management_ip);
            else if (all_ipv4.empty())
                entry->add_ipv4("N/A");

            for (const auto& extra_ipv4 : all_ipv4)
                if (extra_ipv4 != management_ip)
                    entry->add_ipv4(extra_ipv4);
        }
    }

    for (const auto& instance : deleted_instances)
    {
        const auto& name = instance.first;
        auto entry = response.add_instances();
        entry->set_name(name);
        entry->mutable_instance_status()->set_status(mp::InstanceStatus::DELETED);
    }

    server->Write(response);
    status_promise->set_value(grpc::Status::OK);
}
catch (const std::exception& e)
{
    status_promise->set_value(grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, e.what(), ""));
}

void mp::Daemon::networks(const NetworksRequest* request,
                          grpc::ServerReaderWriterInterface<NetworksReply, NetworksRequest>* server,
                          std::promise<grpc::Status>* status_promise) // clang-format off
try // clang-format on
{
    mpl::ClientLogger<NetworksReply, NetworksRequest> logger{mpl::level_from(request->verbosity_level()),
                                                             *config->logger, server};
    NetworksReply response;
    config->update_prompt->populate_if_time_to_show(response.mutable_update_info());

    if (!instances_running(operative_instances))
        config->factory->hypervisor_health_check();

    const auto& iface_list = config->factory->networks();

    for (const auto& iface : iface_list)
    {
        auto entry = response.add_interfaces();
        entry->set_name(iface.id);
        entry->set_type(iface.type);
        entry->set_description(iface.description);
    }

    server->Write(response);
    status_promise->set_value(grpc::Status::OK);
}
catch (const std::exception& e)
{
    status_promise->set_value(grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, e.what(), ""));
}

void mp::Daemon::mount(const MountRequest* request, grpc::ServerReaderWriterInterface<MountReply, MountRequest>* server,
                       std::promise<grpc::Status>* status_promise) // clang-format off
try // clang-format on
{
    mpl::ClientLogger<MountReply, MountRequest> logger{mpl::level_from(request->verbosity_level()), *config->logger,
                                                       server};

    if (!MP_SETTINGS.get_as<bool>(mp::mounts_key))
        return status_promise->set_value(
            grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                         "Mounts are disabled on this installation of Multipass.\n\n"
                         "See https://multipass.run/docs/set-command#local.privileged-mounts for information\n"
                         "on how to enable them."));

    mp::id_mappings uid_mappings, gid_mappings;
    for (const auto& map : request->mount_maps().uid_mappings())
        uid_mappings.push_back({map.host_id(), map.instance_id()});

    for (const auto& map : request->mount_maps().gid_mappings())
        gid_mappings.push_back({map.host_id(), map.instance_id()});

    fmt::memory_buffer errors;
    for (const auto& path_entry : request->target_paths())
    {
        const auto& name = path_entry.instance_name();
        const auto target_path = QDir::cleanPath(QString::fromStdString(path_entry.target_path())).toStdString();

        auto it = operative_instances.find(name);
        if (it == operative_instances.end())
        {
            add_fmt_to(errors, "instance '{}' does not exist", name);
            continue;
        }
        auto& vm = it->second;

        if (mp::utils::invalid_target_path(QString::fromStdString(target_path)))
        {
            add_fmt_to(errors, "unable to mount to \"{}\"", target_path);
            continue;
        }

        auto& vm_mounts = mounts[name];
        if (vm_mounts.find(target_path) != vm_mounts.end())
        {
            add_fmt_to(errors, "\"{}\" is already mounted in '{}'", target_path, name);
            continue;
        }

        const auto mount_type = request->mount_type() == MountRequest_MountType_CLASSIC ? VMMount::MountType::Classic
                                                                                        : VMMount::MountType::Native;

        VMMount vm_mount{request->source_path(), gid_mappings, uid_mappings, mount_type};
        vm_mounts[target_path] = make_mount(vm.get(), target_path, vm_mount);
        if (vm->current_state() == mp::VirtualMachine::State::running ||
            vm_mounts[target_path]->is_mount_managed_by_backend())
        {
            try
            {
                vm_mounts[target_path]->activate(server);
            }
            catch (const mp::SSHFSMissingError&)
            {
                return status_promise->set_value(grpc_status_for_mount_error(name));
            }
            catch (const std::exception& e)
            {
                add_fmt_to(errors, "error mounting \"{}\": {}", target_path, e.what());
                vm_mounts.erase(target_path);
                continue;
            }
        }

        vm_instance_specs[name].mounts[target_path] = vm_mount;
    }

    persist_instances();

    status_promise->set_value(grpc_status_for(errors));
}
catch (const std::exception& e)
{
    status_promise->set_value(grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, e.what(), ""));
}

void mp::Daemon::recover(const RecoverRequest* request,
                         grpc::ServerReaderWriterInterface<RecoverReply, RecoverRequest>* server,
                         std::promise<grpc::Status>* status_promise) // clang-format off
try // clang-format on
{
    mpl::ClientLogger<RecoverReply, RecoverRequest> logger{mpl::level_from(request->verbosity_level()), *config->logger,
                                                           server};

    auto recover_reaction = require_existing_instances_reaction;
    recover_reaction.operative_reaction.message_template = "instance \"{}\" does not need to be recovered";

    auto [instance_selection, status] =
        select_instances_and_react(operative_instances, deleted_instances, request->instance_names().instance_name(),
                                   InstanceGroup::Deleted, recover_reaction);

    if (status.ok())
    {
        for (const auto& vm_it : instance_selection.deleted_selection)
        {
            const auto name = vm_it->first;
            assert(vm_instance_specs[name].deleted);
            vm_instance_specs[name].deleted = false;
            operative_instances[name] = std::move(vm_it->second);
            deleted_instances.erase(vm_it);
            init_mounts(name);
        }
        persist_instances();
    }

    status_promise->set_value(status);
}
catch (const std::exception& e)
{
    status_promise->set_value(grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, e.what(), ""));
}

void mp::Daemon::ssh_info(const SSHInfoRequest* request,
                          grpc::ServerReaderWriterInterface<SSHInfoReply, SSHInfoRequest>* server,
                          std::promise<grpc::Status>* status_promise) // clang-format off
try // clang-format on
{
    mpl::ClientLogger<SSHInfoReply, SSHInfoRequest> logger{mpl::level_from(request->verbosity_level()), *config->logger,
                                                           server};

    auto [instance_selection, status] =
        select_instances_and_react(operative_instances, deleted_instances, request->instance_name(),
                                   InstanceGroup::None, require_operative_instances_reaction);

    if (status.ok())
    {
        SSHInfoReply response;
        auto operation = std::bind(&Daemon::get_ssh_info_for_vm, this, std::placeholders::_1, std::ref(response));
        if ((status = cmd_vms(instance_selection.operative_selection, operation)).ok())
            server->Write(response);
    }

    status_promise->set_value(status);
}
catch (const std::exception& e)
{
    status_promise->set_value(grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, e.what(), ""));
}

void mp::Daemon::start(const StartRequest* request, grpc::ServerReaderWriterInterface<StartReply, StartRequest>* server,
                       std::promise<grpc::Status>* status_promise) // clang-format off
try // clang-format on
{
    mpl::ClientLogger<StartReply, StartRequest> logger{mpl::level_from(request->verbosity_level()), *config->logger,
                                                       server};

    auto timeout = request->timeout() > 0 ? std::chrono::seconds(request->timeout()) : mp::default_timeout;

    if (!instances_running(operative_instances))
        config->factory->hypervisor_health_check();

    const SelectionReaction custom_reaction{
        {grpc::StatusCode::OK}, {grpc::StatusCode::ABORTED}, {grpc::StatusCode::ABORTED}};
    auto [instance_selection, status] =
        select_instances_and_react(operative_instances, deleted_instances, request->instance_names().instance_name(),
                                   InstanceGroup::Operative, custom_reaction);

    if (!status.ok())
        return status_promise->set_value(
            {status.error_code(), "instance(s) missing", make_start_error_details(instance_selection)});

    bool complain_disabled_mounts = !MP_SETTINGS.get_as<bool>(mp::mounts_key);

    std::vector<std::string> starting_vms{};
    starting_vms.reserve(instance_selection.operative_selection.size());

    fmt::memory_buffer start_errors;
    for (auto& vm_it : instance_selection.operative_selection)
    {
        std::lock_guard lock{start_mutex};
        const auto& name = vm_it->first;
        auto& vm = *vm_it->second;
        switch (vm.current_state())
        {
        case VirtualMachine::State::unknown:
        {
            auto error_string = fmt::format("Instance \'{}\' is already running, but in an unknown state", name);
            mpl::log(mpl::Level::warning, category, error_string);
            fmt::format_to(std::back_inserter(start_errors), error_string);
            continue;
        }
        case VirtualMachine::State::suspending:
            fmt::format_to(std::back_inserter(start_errors), "Cannot start the instance \'{}\' while suspending", name);
            continue;
        case VirtualMachine::State::delayed_shutdown:
            delayed_shutdown_instances.erase(name);
            continue;
        case VirtualMachine::State::running:
            continue;
        case VirtualMachine::State::starting:
        case VirtualMachine::State::restarting:
            break;
        default:
            if (complain_disabled_mounts && !vm_instance_specs[name].mounts.empty())
            {
                complain_disabled_mounts = false; // I shall say zis only once
                mpl::log(mpl::Level::error, category, "Mounts have been disabled on this instance of Multipass");
            }

            vm.start();
        }

        starting_vms.push_back(vm_it->first);
    }

    auto future_watcher = create_future_watcher();
    future_watcher->setFuture(QtConcurrent::run(this, &Daemon::async_wait_for_ready_all<StartReply, StartRequest>,
                                                server, starting_vms, timeout, status_promise,
                                                fmt::to_string(start_errors)));
}
catch (const std::exception& e)
{
    status_promise->set_value(grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, e.what(), ""));
}

void mp::Daemon::stop(const StopRequest* request, grpc::ServerReaderWriterInterface<StopReply, StopRequest>* server,
                      std::promise<grpc::Status>* status_promise) // clang-format off
try // clang-format on
{
    mpl::ClientLogger<StopReply, StopRequest> logger{mpl::level_from(request->verbosity_level()), *config->logger,
                                                     server};

    auto [instance_selection, status] =
        select_instances_and_react(operative_instances, deleted_instances, request->instance_names().instance_name(),
                                   InstanceGroup::Operative, require_operative_instances_reaction);

    if (status.ok())
    {
        assert(instance_selection.deleted_selection.empty());
        assert(instance_selection.missing_instances.empty());

        std::function<grpc::Status(VirtualMachine&)> operation;
        if (request->cancel_shutdown())
            operation = std::bind(&Daemon::cancel_vm_shutdown, this, std::placeholders::_1);
        else
            operation = std::bind(&Daemon::shutdown_vm, this, std::placeholders::_1,
                                  std::chrono::minutes(request->time_minutes()));

        status = cmd_vms(instance_selection.operative_selection, operation);
    }

    status_promise->set_value(status);
}
catch (const std::exception& e)
{
    status_promise->set_value(grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, e.what(), ""));
}

void mp::Daemon::suspend(const SuspendRequest* request,
                         grpc::ServerReaderWriterInterface<SuspendReply, SuspendRequest>* server,
                         std::promise<grpc::Status>* status_promise) // clang-format off
try // clang-format on
{
    mpl::ClientLogger<SuspendReply, SuspendRequest> logger{mpl::level_from(request->verbosity_level()), *config->logger,
                                                           server};

    auto [instance_selection, status] =
        select_instances_and_react(operative_instances, deleted_instances, request->instance_names().instance_name(),
                                   InstanceGroup::Operative, require_operative_instances_reaction);

    if (status.ok())
    {
        status = cmd_vms(instance_selection.operative_selection, [this](auto& vm) {
            stop_mounts(vm.vm_name);

            vm.suspend();
            return grpc::Status::OK;
        });
    }

    status_promise->set_value(status);
}
catch (const std::exception& e)
{
    status_promise->set_value(grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, e.what(), ""));
}

void mp::Daemon::restart(const RestartRequest* request,
                         grpc::ServerReaderWriterInterface<RestartReply, RestartRequest>* server,
                         std::promise<grpc::Status>* status_promise) // clang-format off
try // clang-format on
{
    mpl::ClientLogger<RestartReply, RestartRequest> logger{mpl::level_from(request->verbosity_level()), *config->logger,
                                                           server};

    auto timeout = request->timeout() > 0 ? std::chrono::seconds(request->timeout()) : mp::default_timeout;

    auto [instance_selection, status] =
        select_instances_and_react(operative_instances, deleted_instances, request->instance_names().instance_name(),
                                   InstanceGroup::Operative, require_operative_instances_reaction);

    if (!status.ok())
    {
        return status_promise->set_value(status);
    }

    const auto& instance_targets = instance_selection.operative_selection;
    status = cmd_vms(instance_targets, [this](auto& vm) {
        stop_mounts(vm.vm_name);

        return reboot_vm(vm);
    }); // 1st pass to reboot all targets

    if (!status.ok())
    {
        return status_promise->set_value(status);
    }

    auto future_watcher = create_future_watcher();

    future_watcher->setFuture(QtConcurrent::run(this, &Daemon::async_wait_for_ready_all<RestartReply, RestartRequest>,
                                                server, names_from(instance_targets), timeout, status_promise,
                                                std::string()));
}
catch (const std::exception& e)
{
    status_promise->set_value(grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, e.what(), ""));
}

void mp::Daemon::delet(const DeleteRequest* request,
                       grpc::ServerReaderWriterInterface<DeleteReply, DeleteRequest>* server,
                       std::promise<grpc::Status>* status_promise) // clang-format off
try // clang-format on
{
    mpl::ClientLogger<DeleteReply, DeleteRequest> logger{mpl::level_from(request->verbosity_level()), *config->logger,
                                                         server};
    DeleteReply response;

    auto [instance_selection, status] =
        select_instances_and_react(operative_instances, deleted_instances, request->instances_snapshots(),
                                   InstanceGroup::All, require_existing_instances_reaction);

    if (status.ok())
    {
        const bool purge = request->purge();
        auto instance_snapshots_map = map_snapshots_to_instances(request->instances_snapshots());

        for (const auto& vm_it : instance_selection.operative_selection)
        {
            const auto& name = vm_it->first;
            auto& instance = vm_it->second;
            assert(!vm_instance_specs[name].deleted);

            if (instance->current_state() == VirtualMachine::State::delayed_shutdown)
                delayed_shutdown_instances.erase(name);

            mounts[name].clear();
            instance->shutdown();

            if (purge)
            {
                // TODO@snapshots call method to delete snapshots
                /*
                if (const auto& it = instance_snapshots_map.find(name);
                    it == instance_snapshots_map.end() || it.second.empty())
                {
                    // Delete instance and snapshots
                    // release_resources(name);
                    // response.add_purged_instances(name);
                }
                else
                {
                    for (const auto& snapshot_name : instance_snapshots_map[name])
                    {
                        // Delete snapshot
                    }
                }
                */

                release_resources(name);
                response.add_purged_instances(name);
            }
            else
            {
                deleted_instances[name] = std::move(instance);
                vm_instance_specs[name].deleted = true;
            }

            operative_instances.erase(vm_it);
        }

        if (purge)
        {
            for (const auto& vm_it : instance_selection.deleted_selection)
            {
                const auto& name = vm_it->first;
                assert(vm_instance_specs[name].deleted);
                response.add_purged_instances(name);
                release_resources(name);
                deleted_instances.erase(vm_it);
            }
        }

        persist_instances();
    }

    server->Write(response);
    status_promise->set_value(status);
}
catch (const std::exception& e)
{
    status_promise->set_value(grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, e.what(), ""));
}

void mp::Daemon::umount(const UmountRequest* request,
                        grpc::ServerReaderWriterInterface<UmountReply, UmountRequest>* server,
                        std::promise<grpc::Status>* status_promise) // clang-format off
try // clang-format on
{
    mpl::ClientLogger<UmountReply, UmountRequest> logger{mpl::level_from(request->verbosity_level()), *config->logger,
                                                         server};

    fmt::memory_buffer errors;
    for (const auto& path_entry : request->target_paths())
    {
        const auto& name = path_entry.instance_name();
        const auto target_path = QDir::cleanPath(QString::fromStdString(path_entry.target_path())).toStdString();

        if (operative_instances.find(name) == operative_instances.end())
        {
            add_fmt_to(errors, "instance '{}' does not exist", name);
            continue;
        }

        auto& vm_spec_mounts = vm_instance_specs[name].mounts;
        auto& vm_mounts = mounts[name];

        auto do_unmount = [&](auto expiring_it) {
            const auto& [target, mount] = *expiring_it;
            try
            {
                mount->deactivate();
                vm_spec_mounts.erase(target);
                vm_mounts.erase(expiring_it);
            }
            catch (const std::runtime_error& e)
            {
                add_fmt_to(errors, "failed to unmount \"{}\" from '{}': {}", target, name, e.what());
            }
        };

        // Empty target path indicates removing all mounts for the VM instance
        if (target_path.empty())
            for (auto expiring_it = vm_mounts.begin(); expiring_it != vm_mounts.end();)
            {
                // iterator must be advanced before used in order to prevent iterator invalidation caused by deleting
                // from the iterated map
                // expiring_it will be invalidated by do_unmount, so it must not be used after this point
                do_unmount(expiring_it++);
            }
        else if (auto it = vm_mounts.find(target_path); it != vm_mounts.end())
            do_unmount(it);
        else
            add_fmt_to(errors, "path \"{}\" is not mounted in '{}'", target_path, name);
    }

    persist_instances();

    status_promise->set_value(grpc_status_for(errors));
}
catch (const std::exception& e)
{
    status_promise->set_value(grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, e.what(), ""));
}

void mp::Daemon::version(const VersionRequest* request,
                         grpc::ServerReaderWriterInterface<VersionReply, VersionRequest>* server,
                         std::promise<grpc::Status>* status_promise)
{
    mpl::ClientLogger<VersionReply, VersionRequest> logger{mpl::level_from(request->verbosity_level()), *config->logger,
                                                           server};

    VersionReply reply;
    reply.set_version(multipass::version_string);
    config->update_prompt->populate(reply.mutable_update_info());
    server->Write(reply);
    status_promise->set_value(grpc::Status::OK);
}

void mp::Daemon::get(const GetRequest* request, grpc::ServerReaderWriterInterface<GetReply, GetRequest>* server,
                     std::promise<grpc::Status>* status_promise)
try
{
    mpl::ClientLogger<GetReply, GetRequest> logger{mpl::level_from(request->verbosity_level()), *config->logger,
                                                   server};

    GetReply reply;

    auto key = request->key();
    auto val = MP_SETTINGS.get(QString::fromStdString(key)).toStdString();
    mpl::log(mpl::Level::debug, category, fmt::format("Returning setting {}={}", key, val));

    reply.set_value(val);
    server->Write(reply);
    status_promise->set_value(grpc::Status::OK);
}
catch (const mp::UnrecognizedSettingException& e)
{
    status_promise->set_value(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, e.what(), ""));
}
catch (const std::exception& e)
{
    status_promise->set_value(grpc::Status(grpc::StatusCode::INTERNAL, e.what(), ""));
}

void mp::Daemon::set(const SetRequest* request, grpc::ServerReaderWriterInterface<SetReply, SetRequest>* server,
                     std::promise<grpc::Status>* status_promise)
try
{
    mpl::ClientLogger<SetReply, SetRequest> logger{mpl::level_from(request->verbosity_level()), *config->logger,
                                                   server};

    auto key = request->key();
    auto val = request->val();

    mpl::log(mpl::Level::trace, category, fmt::format("Trying to set {}={}", key, val));
    MP_SETTINGS.set(QString::fromStdString(key), QString::fromStdString(val));
    mpl::log(mpl::Level::debug, category, fmt::format("Succeeded setting {}={}", key, val));

    status_promise->set_value(grpc::Status::OK);
}
catch (const mp::UnrecognizedSettingException& e)
{
    status_promise->set_value(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, e.what(), ""));
}
catch (const mp::InvalidSettingException& e)
{
    status_promise->set_value(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, e.what(), ""));
}
catch (const std::exception& e)
{
    status_promise->set_value(grpc::Status(grpc::StatusCode::INTERNAL, e.what(), ""));
}

void mp::Daemon::keys(const mp::KeysRequest* request, grpc::ServerReaderWriterInterface<KeysReply, KeysRequest>* server,
                      std::promise<grpc::Status>* status_promise)
try
{
    mpl::ClientLogger<KeysReply, KeysRequest> logger{mpl::level_from(request->verbosity_level()), *config->logger,
                                                     server};

    KeysReply reply;

    for (const auto& key : MP_SETTINGS.keys())
        reply.add_settings_keys(key.toStdString());

    mpl::log(mpl::Level::debug, category, fmt::format("Returning {} settings keys", reply.settings_keys_size()));
    server->Write(reply);

    status_promise->set_value(grpc::Status::OK);
}
catch (const std::exception& e)
{
    status_promise->set_value(grpc::Status(grpc::StatusCode::INTERNAL, e.what(), ""));
}

void mp::Daemon::authenticate(const AuthenticateRequest* request,
                              grpc::ServerReaderWriterInterface<AuthenticateReply, AuthenticateRequest>* server,
                              std::promise<grpc::Status>* status_promise)
try
{
    mpl::ClientLogger<AuthenticateReply, AuthenticateRequest> logger{mpl::level_from(request->verbosity_level()),
                                                                     *config->logger, server};

    auto stored_hash = MP_SETTINGS.get(mp::passphrase_key);

    if (stored_hash.isNull() || stored_hash.isEmpty())
    {
        return status_promise->set_value(
            grpc::Status(grpc::StatusCode::FAILED_PRECONDITION,
                         "Passphrase is not set. Please `multipass set local.passphrase` with a trusted client."));
    }

    auto hashed_passphrase = MP_UTILS.generate_scrypt_hash_for(QString::fromStdString(request->passphrase()));

    if (stored_hash != hashed_passphrase)
    {
        return status_promise->set_value(
            grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Passphrase is not correct. Please try again."));
    }

    status_promise->set_value(grpc::Status::OK);
}
catch (const std::exception& e)
{
    status_promise->set_value(grpc::Status(grpc::StatusCode::INTERNAL, e.what(), ""));
}

void mp::Daemon::snapshot(const mp::SnapshotRequest* request,
                          grpc::ServerReaderWriterInterface<SnapshotReply, SnapshotRequest>* server,
                          std::promise<grpc::Status>* status_promise)
try
{
    mpl::ClientLogger<SnapshotReply, SnapshotRequest> logger{mpl::level_from(request->verbosity_level()),
                                                             *config->logger, server};

    const auto& instance_name = request->instance();
    auto [instance_trail, status] = find_instance_and_react(operative_instances, deleted_instances, instance_name,
                                                            require_operative_instances_reaction);

    if (status.ok())
    {
        assert(instance_trail.index() == 0);
        auto* vm_ptr = std::get<0>(instance_trail)->second.get();
        assert(vm_ptr);

        using St = VirtualMachine::State;
        if (auto state = vm_ptr->current_state(); state != St::off && state != St::stopped)
            return status_promise->set_value(
                grpc::Status{grpc::INVALID_ARGUMENT, "Multipass can only take snapshots of stopped instances."});

        auto snapshot_name = request->snapshot();
        if (!snapshot_name.empty() && !mp::utils::valid_hostname(snapshot_name))
            return status_promise->set_value(
                grpc::Status{grpc::INVALID_ARGUMENT, fmt::format(R"(Invalid snapshot name: "{}".)", snapshot_name)});

        const auto spec_it = vm_instance_specs.find(instance_name);
        assert(spec_it != vm_instance_specs.end() && "missing instance specs");

        SnapshotReply reply;

        {
            const auto snapshot = vm_ptr->take_snapshot(instance_directory(instance_name, *config), spec_it->second,
                                                        snapshot_name, request->comment());

            reply.set_snapshot(snapshot->get_name());
        }

        server->Write(reply);
    }

    status_promise->set_value(status);
}
catch (const SnapshotNameTaken& e)
{
    status_promise->set_value(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, e.what(), ""));
}
catch (const std::exception& e)
{
    status_promise->set_value(grpc::Status(grpc::StatusCode::INTERNAL, e.what(), ""));
}

void mp::Daemon::restore(const mp::RestoreRequest* request,
                         grpc::ServerReaderWriterInterface<RestoreReply, RestoreRequest>* server,
                         std::promise<grpc::Status>* status_promise)
try
{
    mpl::ClientLogger<RestoreReply, RestoreRequest> logger{mpl::level_from(request->verbosity_level()), *config->logger,
                                                           server};

    RestoreReply reply;
    const auto& instance_name = request->instance();
    auto [instance_trail, status] = find_instance_and_react(operative_instances, deleted_instances, instance_name,
                                                            require_operative_instances_reaction);

    if (status.ok())
    {
        assert(instance_trail.index() == 0);
        auto* vm_ptr = std::get<0>(instance_trail)->second.get();
        assert(vm_ptr);

        using St = VirtualMachine::State;
        if (auto state = vm_ptr->current_state(); state != St::off && state != St::stopped)
            return status_promise->set_value(
                grpc::Status{grpc::INVALID_ARGUMENT, "Multipass can only restore snapshots of stopped instances."});

        auto spec_it = vm_instance_specs.find(instance_name);
        assert(spec_it != vm_instance_specs.end() && "missing instance specs");

        const auto& vm_dir = instance_directory(instance_name, *config);
        if (!request->destructive())
        {
            reply_msg(server, fmt::format("Taking snapshot before restoring {}", instance_name));

            const auto snapshot = vm_ptr->take_snapshot(vm_dir, spec_it->second, "",
                                                        fmt::format("Before restoring {}", request->snapshot()));

            reply_msg(server, fmt::format("Snapshot taken: {}.{}", instance_name, snapshot->get_name()),
                      /* sticky = */ true);
        }

        reply_msg(server, "Restoring snapshot");
        vm_ptr->restore_snapshot(vm_dir, request->snapshot(), spec_it->second);
        persist_instances();

        server->Write(reply);
    }

    status_promise->set_value(status);
}
catch (const std::exception& e)
{
    status_promise->set_value(grpc::Status(grpc::StatusCode::INTERNAL, e.what(), ""));
}

void mp::Daemon::on_shutdown()
{
}

void mp::Daemon::on_resume()
{
}

void mp::Daemon::on_stop()
{
}

void mp::Daemon::on_suspend()
{
}

void mp::Daemon::on_restart(const std::string& name)
{
    stop_mounts(name);
    auto future_watcher = create_future_watcher([this, &name]() {
        auto virtual_machine = operative_instances[name];
        std::lock_guard<decltype(virtual_machine->state_mutex)> lock{virtual_machine->state_mutex};
        virtual_machine->state = VirtualMachine::State::running;
        virtual_machine->update_state();
    });
    future_watcher->setFuture(QtConcurrent::run(this, &Daemon::async_wait_for_ready_all<StartReply, StartRequest>,
                                                nullptr, std::vector<std::string>{name}, mp::default_timeout, nullptr,
                                                std::string()));
}

void mp::Daemon::persist_state_for(const std::string& name, const VirtualMachine::State& state)
{
    vm_instance_specs[name].state = state;
    persist_instances();
}

void mp::Daemon::update_metadata_for(const std::string& name, const QJsonObject& metadata)
{
    vm_instance_specs[name].metadata = metadata;

    persist_instances();
}

QJsonObject mp::Daemon::retrieve_metadata_for(const std::string& name)
{
    return vm_instance_specs[name].metadata;
}

void mp::Daemon::persist_instances()
{
    QJsonObject instance_records_json;
    for (const auto& record : vm_instance_specs)
    {
        auto key = QString::fromStdString(record.first);
        instance_records_json.insert(key, vm_spec_to_json(record.second));
    }
    QDir data_dir{
        mp::utils::backend_directory_path(config->data_directory, config->factory->get_backend_directory_name())};
    mp::write_json(instance_records_json, data_dir.filePath(instance_db_name));
}

void mp::Daemon::release_resources(const std::string& instance)
{
    config->factory->remove_resources_for(instance);
    config->vault->remove(instance);

    auto spec_it = vm_instance_specs.find(instance);
    if (spec_it != cend(vm_instance_specs))
    {
        for (const auto& mac : mac_set_from(spec_it->second))
            allocated_mac_addrs.erase(mac);

        vm_instance_specs.erase(spec_it);
    }
}

void mp::Daemon::create_vm(const CreateRequest* request,
                           grpc::ServerReaderWriterInterface<CreateReply, CreateRequest>* server,
                           std::promise<grpc::Status>* status_promise, bool start)
{
    typedef typename std::pair<VirtualMachineDescription, ClientLaunchData> VMFullDescription;

    auto checked_args = validate_create_arguments(request, config.get());

    if (!checked_args.option_errors.error_codes().empty())
    {
        return status_promise->set_value(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Invalid arguments supplied",
                                                      checked_args.option_errors.SerializeAsString()));
    }
    else if (auto& nets = checked_args.nets_need_bridging; !nets.empty() && !request->permission_to_bridge())
    {
        CreateError create_error;
        create_error.add_error_codes(CreateError::INVALID_NETWORK);

        CreateReply reply;
        *reply.mutable_nets_need_bridging() = {std::make_move_iterator(nets.begin()),
                                               std::make_move_iterator(nets.end())}; /* this constructs a temporary
                                               RepeatedPtrField from the range, then move-assigns that temporary in */
        server->Write(reply);

        return status_promise->set_value(
            grpc::Status{grpc::StatusCode::FAILED_PRECONDITION, "Missing bridges", create_error.SerializeAsString()});
    }

    // TODO: We should only need to query the Blueprint Provider once for all info, so this (and timeout below) will
    //       need a refactoring to do so.
    const std::string blueprint_name = config->blueprint_provider->name_from_blueprint(request->image());
    auto name = name_from(checked_args.instance_name, blueprint_name, *config->name_generator, operative_instances);

    auto [instance_trail, status] =
        find_instance_and_react(operative_instances, deleted_instances, name, require_missing_instances_reaction);

    assert(status.ok() == (instance_trail.index() == 2));
    if (!status.ok())
        return status_promise->set_value(status);

    if (preparing_instances.find(name) != preparing_instances.end())
        return status_promise->set_value(
            {grpc::StatusCode::INVALID_ARGUMENT, fmt::format("instance \"{}\" is being prepared", name), ""});

    if (!instances_running(operative_instances))
        config->factory->hypervisor_health_check();

    // TODO: We should only need to query the Blueprint Provider once for all info, so this (and name above) will
    //       need a refactoring to do so.
    auto timeout = timeout_for(request->timeout(), config->blueprint_provider->blueprint_timeout(blueprint_name));

    preparing_instances.insert(name);

    auto prepare_future_watcher = new QFutureWatcher<VMFullDescription>();
    auto log_level = mpl::level_from(request->verbosity_level());

    QObject::connect(
        prepare_future_watcher, &QFutureWatcher<VMFullDescription>::finished,
        [this, server, status_promise, name, timeout, start, prepare_future_watcher, log_level] {
            mpl::ClientLogger<CreateReply, CreateRequest> logger{log_level, *config->logger, server};

            try
            {
                auto vm_desc_pair = prepare_future_watcher->future().result();
                auto vm_desc = vm_desc_pair.first;
                auto& vm_client_data = vm_desc_pair.second;
                auto& vm_aliases = vm_client_data.aliases_to_be_created;
                auto& vm_workspaces = vm_client_data.workspaces_to_be_created;

                vm_instance_specs[name] = {vm_desc.num_cores,
                                           vm_desc.mem_size,
                                           vm_desc.disk_space,
                                           vm_desc.default_mac_address,
                                           vm_desc.extra_interfaces,
                                           config->ssh_username,
                                           VirtualMachine::State::off,
                                           {},
                                           false,
                                           QJsonObject()};
                operative_instances[name] = config->factory->create_virtual_machine(vm_desc, *this);
                preparing_instances.erase(name);

                persist_instances();

                if (start)
                {
                    LaunchReply reply;
                    reply.set_create_message("Starting " + name);
                    server->Write(reply);

                    operative_instances[name]->start();

                    auto future_watcher = create_future_watcher([this, server, name, vm_aliases, vm_workspaces] {
                        LaunchReply reply;
                        reply.set_vm_instance_name(name);
                        config->update_prompt->populate_if_time_to_show(reply.mutable_update_info());

                        // Attach the aliases to be created by the CLI to the last message.
                        for (const auto& blueprint_alias : vm_aliases)
                        {
                            mpl::log(mpl::Level::debug, category,
                                     fmt::format("Adding alias '{}' to RPC reply", blueprint_alias.first));
                            auto alias = reply.add_aliases_to_be_created();
                            alias->set_name(blueprint_alias.first);
                            alias->set_instance(blueprint_alias.second.instance);
                            alias->set_command(blueprint_alias.second.command);
                            alias->set_working_directory(blueprint_alias.second.working_directory);
                        }

                        // Now attach the workspaces.
                        for (const auto& blueprint_workspace : vm_workspaces)
                        {
                            mpl::log(mpl::Level::debug, category,
                                     fmt::format("Adding workspace '{}' to RPC reply", blueprint_workspace));
                            reply.add_workspaces_to_be_created(blueprint_workspace);
                        }

                        server->Write(reply);
                    });
                    future_watcher->setFuture(
                        QtConcurrent::run(this, &Daemon::async_wait_for_ready_all<LaunchReply, LaunchRequest>, server,
                                          std::vector<std::string>{name}, timeout, status_promise, std::string()));
                }
                else
                {
                    status_promise->set_value(grpc::Status::OK);
                }
            }
            catch (const std::exception& e)
            {
                preparing_instances.erase(name);
                release_resources(name);
                operative_instances.erase(name);
                persist_instances();
                status_promise->set_value(grpc::Status(grpc::StatusCode::FAILED_PRECONDITION, e.what(), ""));
            }

            delete prepare_future_watcher;
        });

    auto make_vm_description = [this, server, request, name, checked_args, log_level]() mutable -> VMFullDescription {
        mpl::ClientLogger<CreateReply, CreateRequest> logger{log_level, *config->logger, server};

        try
        {
            CreateReply reply;
            reply.set_create_message("Creating " + name);
            server->Write(reply);

            Query query;
            VirtualMachineDescription vm_desc{
                request->num_cores(),
                MemorySize{request->mem_size().empty() ? "0b" : request->mem_size()},
                MemorySize{request->disk_space().empty() ? "0b" : request->disk_space()},
                name,
                "",
                {},
                config->ssh_username,
                VMImage{},
                "",
                YAML::Node{},
                YAML::Node{},
                make_cloud_init_vendor_config(*config->ssh_key_provider, config->ssh_username,
                                              config->factory->get_backend_version_string().toStdString(), request),
                YAML::Node{}};

            ClientLaunchData client_launch_data;

            bool launch_from_blueprint{true};
            try
            {
                auto image = request->image();
                auto image_qstr = QString::fromStdString(image);

                // If requesting to launch from a yaml file, we assume it contains a Blueprint.
                if (image_qstr.startsWith("file://") &&
                    (image_qstr.toLower().endsWith(".yaml") || image_qstr.toLower().endsWith(".yml")))
                {
                    auto file_info = QFileInfo(image_qstr.remove(0, 7));
                    auto file_path = file_info.absoluteFilePath();

                    auto chop = image_qstr.at(image_qstr.size() - 4) == '.' ? 4 : 5;
                    image = file_info.fileName().chopped(chop).toStdString();

                    query = config->blueprint_provider->blueprint_from_file(file_path.toStdString(), image, vm_desc,
                                                                            client_launch_data);
                }
                else
                {
                    query = config->blueprint_provider->fetch_blueprint_for(image, vm_desc, client_launch_data);
                }

                query.name = name;

                // Aliases and default workspace are named in function of the instance name in the Blueprint. If the
                // user asked for a different name, it will be necessary to change the alias definitions and the
                // workspace name to reflect it.
                if (name != image)
                {
                    for (auto& alias_to_define : client_launch_data.aliases_to_be_created)
                        if (alias_to_define.second.instance == image)
                        {
                            mpl::log(mpl::Level::trace, category,
                                     fmt::format("Renaming instance on alias \"{}\" from \"{}\" to \"{}\"",
                                                 alias_to_define.first, alias_to_define.second.instance, name));
                            alias_to_define.second.instance = name;
                        }

                    for (auto& workspace_to_create : client_launch_data.workspaces_to_be_created)
                        if (workspace_to_create == image)
                        {
                            mpl::log(mpl::Level::trace, category,
                                     fmt::format("Renaming workspace \"{}\" to \"{}\"", workspace_to_create, name));
                            workspace_to_create = name;
                        }
                }
            }
            catch (const std::out_of_range&)
            {
                // Blueprint not found, move on
                launch_from_blueprint = false;
                query = query_from(request, name);
                vm_desc.mem_size = checked_args.mem_size;
            }

            auto progress_monitor = [server](int progress_type, int percentage) {
                CreateReply create_reply;
                create_reply.mutable_launch_progress()->set_percent_complete(std::to_string(percentage));
                create_reply.mutable_launch_progress()->set_type((CreateProgress::ProgressTypes)progress_type);
                return server->Write(create_reply);
            };

            auto prepare_action = [this, server, &name](const VMImage& source_image) -> VMImage {
                CreateReply reply;
                reply.set_create_message("Preparing image for " + name);
                server->Write(reply);

                return config->factory->prepare_source_image(source_image);
            };

            auto fetch_type = config->factory->fetch_type();

            std::optional<std::string> checksum;
            if (!vm_desc.image.id.empty())
                checksum = vm_desc.image.id;

            auto vm_image = config->vault->fetch_image(fetch_type, query, prepare_action, progress_monitor,
                                                       launch_from_blueprint, checksum);

            const auto image_size = config->vault->minimum_image_size_for(vm_image.id);
            vm_desc.disk_space = compute_final_image_size(
                image_size, vm_desc.disk_space.in_bytes() > 0 ? vm_desc.disk_space : checked_args.disk_space,
                config->data_directory);

            reply.set_create_message("Configuring " + name);
            server->Write(reply);

            config->factory->prepare_networking(checked_args.extra_interfaces);

            // This set stores the MAC's which need to be in the allocated_mac_addrs if everything goes well.
            auto new_macs = allocated_mac_addrs;

            // check for repetition of requested macs
            for (auto& iface : checked_args.extra_interfaces)
                if (!iface.mac_address.empty() && !new_macs.insert(iface.mac_address).second)
                    throw std::runtime_error(fmt::format("Repeated MAC address {}", iface.mac_address));

            // generate missing macs in a second pass, to avoid repeating macs that the user requested
            for (auto& iface : checked_args.extra_interfaces)
                if (iface.mac_address.empty())
                    iface.mac_address = generate_unused_mac_address(new_macs);

            vm_desc.default_mac_address = generate_unused_mac_address(new_macs);
            vm_desc.extra_interfaces = checked_args.extra_interfaces;

            vm_desc.meta_data_config = make_cloud_init_meta_config(name);
            vm_desc.user_data_config = YAML::Load(request->cloud_init_user_data());
            prepare_user_data(vm_desc.user_data_config, vm_desc.vendor_data_config);

            if (vm_desc.num_cores < std::stoi(mp::min_cpu_cores))
                vm_desc.num_cores = std::stoi(mp::default_cpu_cores);

            vm_desc.network_data_config =
                make_cloud_init_network_config(vm_desc.default_mac_address, checked_args.extra_interfaces);

            vm_desc.image = vm_image;
            config->factory->configure(vm_desc);
            config->factory->prepare_instance_image(vm_image, vm_desc);

            // Everything went well, add the MAC addresses used in this instance.
            allocated_mac_addrs = std::move(new_macs);

            return VMFullDescription{vm_desc, client_launch_data};
        }
        catch (const std::exception& e)
        {
            throw CreateImageException(e.what());
        }
    };

    prepare_future_watcher->setFuture(QtConcurrent::run(make_vm_description));
}

grpc::Status mp::Daemon::reboot_vm(VirtualMachine& vm)
{
    if (vm.state == VirtualMachine::State::delayed_shutdown)
        delayed_shutdown_instances.erase(vm.vm_name);

    if (!mp::utils::is_running(vm.current_state()))
        return grpc::Status{grpc::StatusCode::INVALID_ARGUMENT,
                            fmt::format("instance \"{}\" is not running", vm.vm_name), ""};

    mpl::log(mpl::Level::debug, category, fmt::format("Rebooting {}", vm.vm_name));
    return ssh_reboot(vm.ssh_hostname(), vm.ssh_port(), vm.ssh_username(), *config->ssh_key_provider);
}

grpc::Status mp::Daemon::shutdown_vm(VirtualMachine& vm, const std::chrono::milliseconds delay)
{
    const auto& name = vm.vm_name;
    const auto& state = vm.current_state();

    using St = VirtualMachine::State;
    const auto skip_states = {St::off, St::stopped, St::suspended};

    if (std::none_of(cbegin(skip_states), cend(skip_states), [&state](const auto& st) { return state == st; }))
    {
        delayed_shutdown_instances.erase(name);

        std::optional<mp::SSHSession> session;
        try
        {
            session = mp::SSHSession{vm.ssh_hostname(), vm.ssh_port(), vm.ssh_username(), *config->ssh_key_provider};
        }
        catch (const std::exception& e)
        {
            mpl::log(mpl::Level::info, category,
                     fmt::format("Cannot open ssh session on \"{}\" shutdown: {}", name, e.what()));
        }

        auto stop_all_mounts = [this](const std::string& name) { stop_mounts(name); };
        auto& shutdown_timer = delayed_shutdown_instances[name] =
            std::make_unique<DelayedShutdownTimer>(&vm, std::move(session), stop_all_mounts);

        QObject::connect(shutdown_timer.get(), &DelayedShutdownTimer::finished,
                         [this, name]() { delayed_shutdown_instances.erase(name); });

        shutdown_timer->start(delay);
    }
    else
        mpl::log(mpl::Level::debug, category, fmt::format("instance \"{}\" does not need stopping", name));

    return grpc::Status::OK;
}

grpc::Status mp::Daemon::cancel_vm_shutdown(const VirtualMachine& vm)
{
    auto it = delayed_shutdown_instances.find(vm.vm_name);
    if (it != delayed_shutdown_instances.end())
        delayed_shutdown_instances.erase(it);
    else
        mpl::log(mpl::Level::debug, category,
                 fmt::format("no delayed shutdown to cancel on instance \"{}\"", vm.vm_name));

    return grpc::Status::OK;
}

grpc::Status mp::Daemon::get_ssh_info_for_vm(VirtualMachine& vm, SSHInfoReply& response)
{
    const auto& name = vm.vm_name;
    if (vm.current_state() == VirtualMachine::State::unknown)
        throw std::runtime_error("Cannot retrieve credentials in unknown state");

    if (!mp::utils::is_running(vm.current_state()))
        return grpc::Status{grpc::StatusCode::ABORTED, fmt::format("instance \"{}\" is not running", name)};

    if (vm.state == VirtualMachine::State::delayed_shutdown &&
        delayed_shutdown_instances[name]->get_time_remaining() <= std::chrono::minutes(1))
        return grpc::Status{grpc::StatusCode::FAILED_PRECONDITION,
                            fmt::format("\"{}\" is scheduled to shut down in less than a minute, use "
                                        "'multipass stop --cancel {}' to cancel the shutdown.",
                                        name, name),
                            ""};

    mp::SSHInfo ssh_info;
    ssh_info.set_host(vm.ssh_hostname());
    ssh_info.set_port(vm.ssh_port());
    ssh_info.set_priv_key_base64(config->ssh_key_provider->private_key_as_base64());
    ssh_info.set_username(vm.ssh_username());
    (*response.mutable_ssh_info())[name] = ssh_info;

    return grpc::Status::OK;
}

void mp::Daemon::init_mounts(const std::string& name)
{
    auto& vm_mounts = mounts[name];
    auto& vm_spec_mounts = vm_instance_specs[name].mounts;
    std::vector<std::string> mounts_to_remove;
    for (const auto& [target, vm_mount] : vm_spec_mounts)
    {
        if (vm_mounts.find(target) == vm_mounts.end())
            try
            {
                vm_mounts[target] = make_mount(operative_instances[name].get(), target, vm_mount);
            }
            catch (const std::exception& e)
            {
                mpl::log(mpl::Level::warning, category,
                         fmt::format(R"(Removing mount "{}" => "{}" from '{}': {})", vm_mount.source_path, target, name,
                                     e.what()));
                mounts_to_remove.push_back(target);
            }
    }

    for (const auto& mount_target : mounts_to_remove)
        vm_spec_mounts.erase(mount_target);

    if (!mounts_to_remove.empty())
        persist_instances();
}

void mp::Daemon::stop_mounts(const std::string& name)
{
    for (auto& [_, mount] : mounts[name])
    {
        if (!mount->is_mount_managed_by_backend())
        {
            mount->deactivate(/*force=*/true);
        }
    }
}

mp::MountHandler::UPtr mp::Daemon::make_mount(VirtualMachine* vm, const std::string& target, const VMMount& mount)
{
    return mount.mount_type == VMMount::MountType::Classic
               ? std::make_unique<SSHFSMountHandler>(vm, config->ssh_key_provider.get(), target, mount)
               : vm->make_native_mount_handler(config->ssh_key_provider.get(), target, mount);
}

QFutureWatcher<mp::Daemon::AsyncOperationStatus>*
mp::Daemon::create_future_watcher(std::function<void()> const& finished_op)
{
    async_future_watchers.emplace_back(std::make_unique<QFutureWatcher<AsyncOperationStatus>>());

    auto future_watcher = async_future_watchers.back().get();
    QObject::connect(future_watcher, &QFutureWatcher<AsyncOperationStatus>::finished,
                     [this, future_watcher, finished_op] {
                         finished_op();
                         finish_async_operation(future_watcher->future());
                     });

    return future_watcher;
}

template <typename Reply, typename Request>
error_string
mp::Daemon::async_wait_for_ssh_and_start_mounts_for(const std::string& name, const std::chrono::seconds& timeout,
                                                    grpc::ServerReaderWriterInterface<Reply, Request>* server)
{
    fmt::memory_buffer errors;
    try
    {
        auto it = operative_instances.find(name);
        auto vm = it->second;
        vm->wait_until_ssh_up(timeout);

        if (std::is_same<Reply, LaunchReply>::value)
        {
            if (server)
            {
                Reply reply;
                reply.set_reply_message("Waiting for initialization to complete");
                server->Write(reply);
            }

            MP_UTILS.wait_for_cloud_init(vm.get(), timeout, *config->ssh_key_provider);
        }

        if (MP_SETTINGS.get_as<bool>(mp::mounts_key))
        {
            std::vector<std::string> invalid_mounts;
            fmt::memory_buffer warnings;
            auto& vm_mounts = mounts[name];
            for (auto& [target, mount] : vm_mounts)
                try
                {
                    if (!mount->is_mount_managed_by_backend())
                    {
                        mount->activate(server);
                    }
                }
                catch (const mp::SSHFSMissingError&)
                {
                    add_fmt_to(errors, sshfs_error_template, name);
                    break;
                }
                catch (const std::exception& e)
                {
                    auto msg = fmt::format("Removing mount \"{}\" from '{}': {}\n", target, name, e.what());
                    mpl::log(mpl::Level::warning, category, msg);
                    fmt::format_to(std::back_inserter(warnings), msg);
                    invalid_mounts.push_back(target);
                }

            auto& vm_spec_mounts = vm_instance_specs[name].mounts;
            for (const auto& target : invalid_mounts)
            {
                vm_mounts.erase(target);
                vm_spec_mounts.erase(target);
            }

            if (server && warnings.size() > 0)
            {
                Reply reply;
                reply.set_log_line(fmt::to_string(warnings));
                server->Write(reply);
            }

            persist_instances();
        }
    }
    catch (const std::exception& e)
    {
        fmt::format_to(std::back_inserter(errors), e.what());
    }

    return fmt::to_string(errors);
}

template <typename Reply, typename Request>
mp::Daemon::AsyncOperationStatus
mp::Daemon::async_wait_for_ready_all(grpc::ServerReaderWriterInterface<Reply, Request>* server,
                                     const std::vector<std::string>& vms, const std::chrono::seconds& timeout,
                                     std::promise<grpc::Status>* status_promise, const std::string& start_errors)
{
    fmt::memory_buffer errors;
    fmt::format_to(std::back_inserter(errors), "{}", start_errors);

    QFutureSynchronizer<std::string> start_synchronizer;
    {
        std::lock_guard<decltype(start_mutex)> lock{start_mutex};
        for (const auto& name : vms)
        {
            if (async_running_futures.find(name) != async_running_futures.end())
            {
                start_synchronizer.addFuture(async_running_futures[name]);
            }
            else
            {
                auto future = QtConcurrent::run(this, &Daemon::async_wait_for_ssh_and_start_mounts_for<Reply, Request>,
                                                name, timeout, server);
                async_running_futures[name] = future;
                start_synchronizer.addFuture(future);
            }
        }
    }

    start_synchronizer.waitForFinished();

    {
        std::lock_guard<decltype(start_mutex)> lock{start_mutex};
        for (const auto& name : vms)
        {
            async_running_futures.erase(name);
        }
    }

    for (const auto& future : start_synchronizer.futures())
        if (auto error = future.result(); !error.empty())
            add_fmt_to(errors, error);

    if (server && std::is_same<Reply, StartReply>::value)
    {
        if (config->update_prompt->is_time_to_show())
        {
            Reply reply;
            config->update_prompt->populate(reply.mutable_update_info());
            server->Write(reply);
        }
    }

    return {grpc_status_for(errors), status_promise};
}

void mp::Daemon::finish_async_operation(QFuture<AsyncOperationStatus> async_future)
{
    auto it = std::find_if(async_future_watchers.begin(), async_future_watchers.end(),
                           [&async_future](const std::unique_ptr<QFutureWatcher<AsyncOperationStatus>>& watcher) {
                               return watcher->future() == async_future;
                           });

    if (it != async_future_watchers.end())
    {
        async_future_watchers.erase(it);
    }

    auto async_op_result = async_future.result();

    if (!async_op_result.status.ok())
        persist_instances();

    if (async_op_result.status_promise)
        async_op_result.status_promise->set_value(async_op_result.status);
}

template <typename Reply, typename Request>
void mp::Daemon::reply_msg(grpc::ServerReaderWriterInterface<Reply, Request>* server, std::string&& msg, bool sticky)
{
    Reply reply{};
    if (sticky)
        reply.set_reply_message(fmt::format("{}\n", std::forward<decltype(msg)>(msg)));
    else
        reply.set_reply_message(std::forward<decltype(msg)>(msg));

    server->Write(reply);
}