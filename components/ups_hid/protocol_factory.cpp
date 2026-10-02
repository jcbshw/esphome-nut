#include "protocol_factory.h"
#include "ups_hid.h"
#include "esphome/core/log.h"
#include <algorithm>

namespace esphome {
namespace ups_hid {

static const char *const FACTORY_TAG = "ups_hid.factory";

// Static registry implementations
std::unordered_map<uint16_t, std::vector<ProtocolFactory::ProtocolInfo>>& 
ProtocolFactory::get_vendor_registry() {
    static std::unordered_map<uint16_t, std::vector<ProtocolInfo>> vendor_registry;
    return vendor_registry;
}

std::vector<ProtocolFactory::ProtocolInfo>& 
ProtocolFactory::get_fallback_registry() {
    static std::vector<ProtocolInfo> fallback_registry;
    return fallback_registry;
}

// Creator functions for the bundled protocols. Declared here rather than in a
// header so that protocol_factory.cpp carries an undefined reference to each
// one; see register_builtin_protocols() in protocol_factory.h for why that
// matters.
std::unique_ptr<UpsProtocolBase> create_cyberpower_protocol(UpsHidComponent* parent);
std::unique_ptr<UpsProtocolBase> create_apc_protocol(UpsHidComponent* parent);
std::unique_ptr<UpsProtocolBase> create_generic_protocol(UpsHidComponent* parent);

void ProtocolFactory::ensure_initialized() {
    static bool initialized = false;
    if (initialized) {
        return;
    }
    // Set before registering: the register_* calls below re-enter this
    // function, and this flag is what stops that recursing.
    initialized = true;

    // NOTE: nothing on this path may log. Once the protocol translation units
    // are linked in, their REGISTER_UPS_* static registrars run during static
    // initialization, which re-enters here long before App.setup() constructs
    // the Logger. esp_log_printf_() only null-checks logger::global_logger
    // under ESPHOME_DEBUG, so a log call here dereferences null and resets the
    // device. Registration is reported from create_for_vendor() instead.
    register_builtin_protocols();
}

void ProtocolFactory::register_builtin_protocols() {
    ProtocolInfo cyberpower;
    cyberpower.creator = create_cyberpower_protocol;
    cyberpower.name = "CyberPower HID Protocol";
    cyberpower.description = "CyberPower CP series HID protocol with comprehensive sensor support and test functionality";
    cyberpower.supported_vendors = {0x0764};
    cyberpower.priority = 100;
    register_protocol_for_vendor(0x0764, cyberpower);

    ProtocolInfo apc;
    apc.creator = create_apc_protocol;
    apc.name = "APC HID Protocol";
    apc.description = "APC Back-UPS and Smart-UPS HID protocol implementation with comprehensive sensor support";
    apc.supported_vendors = {0x051D};
    apc.priority = 100;
    register_protocol_for_vendor(0x051D, apc);

    ProtocolInfo generic;
    generic.creator = create_generic_protocol;
    generic.name = "Generic HID Protocol";
    generic.description = "Universal HID protocol fallback for unknown UPS vendors with basic monitoring capabilities";
    generic.priority = 10;
    register_fallback_protocol(generic);
}

void ProtocolFactory::register_protocol_for_vendor(uint16_t vendor_id, 
                                                  const ProtocolInfo& info) {
    ensure_initialized();
    
    auto& registry = get_vendor_registry();

    // The macro registrar for a protocol fires as well as the explicit
    // registration in register_builtin_protocols(); keep only the first.
    auto& entries = registry[vendor_id];
    if (std::any_of(entries.begin(), entries.end(),
                    [&info](const ProtocolInfo& existing) { return existing.name == info.name; })) {
        return;
    }

    entries.push_back(info);

    // Sort by priority (higher first)
    std::sort(registry[vendor_id].begin(), registry[vendor_id].end(),
              [](const ProtocolInfo& a, const ProtocolInfo& b) {
                  return a.priority > b.priority;
              });
    // Deliberately no logging here - see ensure_initialized().
}

void ProtocolFactory::register_fallback_protocol(const ProtocolInfo& info) {
    ensure_initialized();
    
    auto& registry = get_fallback_registry();

    // Same duplicate suppression as the vendor registry above.
    if (std::any_of(registry.begin(), registry.end(),
                    [&info](const ProtocolInfo& existing) { return existing.name == info.name; })) {
        return;
    }

    registry.push_back(info);

    // Sort by priority (higher first)
    std::sort(registry.begin(), registry.end(),
              [](const ProtocolInfo& a, const ProtocolInfo& b) {
                  return a.priority > b.priority;
              });
    // Deliberately no logging here - see ensure_initialized().
}

std::unique_ptr<UpsProtocolBase> 
ProtocolFactory::create_for_vendor(uint16_t vendor_id, UpsHidComponent* parent) {
    ensure_initialized();
    
    if (!parent) {
        ESP_LOGE(FACTORY_TAG, "Cannot create protocol with null parent component");
        return nullptr;
    }

    // Registration happens during static init, where logging is unsafe, so
    // report what ended up registered on the first runtime call instead.
    static bool reported = false;
    if (!reported) {
        reported = true;
        size_t vendor_count = 0;
        for (const auto& entry : get_vendor_registry()) {
            vendor_count += entry.second.size();
        }
        ESP_LOGI(FACTORY_TAG, "Protocol registry: %zu vendor-specific, %zu fallback",
                 vendor_count, get_fallback_registry().size());
    }

    // Try vendor-specific protocols first
    auto& vendor_registry = get_vendor_registry();
    auto vendor_it = vendor_registry.find(vendor_id);
    
    if (vendor_it != vendor_registry.end()) {
        ESP_LOGD(FACTORY_TAG, "Found %zu vendor-specific protocols for 0x%04X", 
                 vendor_it->second.size(), vendor_id);
        
        for (const auto& info : vendor_it->second) {
            ESP_LOGD(FACTORY_TAG, "Trying vendor protocol '%s' for 0x%04X", 
                     info.name.c_str(), vendor_id);
            
            auto protocol = info.creator(parent);
            if (protocol && protocol->detect()) {
                ESP_LOGI(FACTORY_TAG, "Successfully created protocol '%s' for vendor 0x%04X", 
                         info.name.c_str(), vendor_id);
                return protocol;
            }
        }
    }
    
    // Try fallback protocols
    auto& fallback_registry = get_fallback_registry();
    ESP_LOGD(FACTORY_TAG, "Trying %zu fallback protocols for vendor 0x%04X", 
             fallback_registry.size(), vendor_id);
    
    for (const auto& info : fallback_registry) {
        ESP_LOGD(FACTORY_TAG, "Trying fallback protocol '%s' for 0x%04X", 
                 info.name.c_str(), vendor_id);
        
        auto protocol = info.creator(parent);
        if (protocol && protocol->detect()) {
            ESP_LOGI(FACTORY_TAG, "Successfully created fallback protocol '%s' for vendor 0x%04X", 
                     info.name.c_str(), vendor_id);
            return protocol;
        }
    }
    
    ESP_LOGW(FACTORY_TAG, "No suitable protocol found for vendor 0x%04X", vendor_id);
    return nullptr;
}

std::vector<ProtocolFactory::ProtocolInfo> 
ProtocolFactory::get_protocols_for_vendor(uint16_t vendor_id) {
    ensure_initialized();
    
    std::vector<ProtocolInfo> protocols;
    
    // Add vendor-specific protocols first
    auto& vendor_registry = get_vendor_registry();
    auto vendor_it = vendor_registry.find(vendor_id);
    
    if (vendor_it != vendor_registry.end()) {
        for (const auto& info : vendor_it->second) {
            protocols.push_back(info);
        }
    }
    
    // Add fallback protocols
    auto& fallback_registry = get_fallback_registry();
    for (const auto& info : fallback_registry) {
        protocols.push_back(info);
    }
    
    return protocols;
}

std::vector<std::pair<uint16_t, ProtocolFactory::ProtocolInfo>> 
ProtocolFactory::get_all_protocols() {
    ensure_initialized();
    
    std::vector<std::pair<uint16_t, ProtocolInfo>> all_protocols;
    
    // Add vendor-specific protocols
    auto& vendor_registry = get_vendor_registry();
    for (const auto& vendor_pair : vendor_registry) {
        uint16_t vendor_id = vendor_pair.first;
        for (const auto& info : vendor_pair.second) {
            all_protocols.emplace_back(vendor_id, info);
        }
    }
    
    // Add fallback protocols (use 0x0000 as special vendor ID for fallbacks)
    auto& fallback_registry = get_fallback_registry();
    for (const auto& info : fallback_registry) {
        all_protocols.emplace_back(0x0000, info);
    }
    
    return all_protocols;
}

bool ProtocolFactory::has_vendor_support(uint16_t vendor_id) {
    ensure_initialized();
    
    auto& vendor_registry = get_vendor_registry();
    auto it = vendor_registry.find(vendor_id);
    
    // Has support if vendor-specific protocols exist OR fallback protocols exist
    bool has_vendor_specific = (it != vendor_registry.end() && !it->second.empty());
    bool has_fallback = !get_fallback_registry().empty();
    
    return has_vendor_specific || has_fallback;
}

std::unique_ptr<UpsProtocolBase> 
ProtocolFactory::create_by_name(const std::string& protocol_name, UpsHidComponent* parent) {
    ensure_initialized();
    
    if (!parent) {
        ESP_LOGE(FACTORY_TAG, "Cannot create protocol with null parent component");
        return nullptr;
    }
    
    ESP_LOGD(FACTORY_TAG, "Creating protocol by name: %s", protocol_name.c_str());
    
    // Search through all registered protocols to find one with matching name
    auto& vendor_registry = get_vendor_registry();
    for (const auto& vendor_pair : vendor_registry) {
        for (const auto& info : vendor_pair.second) {
            // Match protocol name (case-insensitive)
            std::string info_name_lower = info.name;
            std::string protocol_name_lower = protocol_name;
            std::transform(info_name_lower.begin(), info_name_lower.end(), info_name_lower.begin(), ::tolower);
            std::transform(protocol_name_lower.begin(), protocol_name_lower.end(), protocol_name_lower.begin(), ::tolower);
            
            if (info_name_lower.find(protocol_name_lower) != std::string::npos) {
                ESP_LOGD(FACTORY_TAG, "Found matching protocol '%s' for name '%s'", 
                         info.name.c_str(), protocol_name.c_str());
                auto protocol = info.creator(parent);
                if (protocol) {
                    ESP_LOGI(FACTORY_TAG, "Successfully created protocol '%s' by name", 
                             protocol->get_protocol_name().c_str());
                    return protocol;
                }
            }
        }
    }
    
    // Search through fallback protocols
    auto& fallback_registry = get_fallback_registry();
    for (const auto& info : fallback_registry) {
        std::string info_name_lower = info.name;
        std::string protocol_name_lower = protocol_name;
        std::transform(info_name_lower.begin(), info_name_lower.end(), info_name_lower.begin(), ::tolower);
        std::transform(protocol_name_lower.begin(), protocol_name_lower.end(), protocol_name_lower.begin(), ::tolower);
        
        if (info_name_lower.find(protocol_name_lower) != std::string::npos) {
            ESP_LOGD(FACTORY_TAG, "Found matching fallback protocol '%s' for name '%s'", 
                     info.name.c_str(), protocol_name.c_str());
            auto protocol = info.creator(parent);
            if (protocol) {
                ESP_LOGI(FACTORY_TAG, "Successfully created fallback protocol '%s' by name", 
                         protocol->get_protocol_name().c_str());
                return protocol;
            }
        }
    }
    
    ESP_LOGE(FACTORY_TAG, "No protocol found with name containing '%s'", protocol_name.c_str());
    return nullptr;
}

} // namespace ups_hid
} // namespace esphome