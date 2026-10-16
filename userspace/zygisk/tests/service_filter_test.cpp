#include "service_filter.h"

#include <cassert>
#include <string>

int main() {
  yukari::FilterConfig config;
  config.target_uids = {10234, 1010234};
  config.target_processes = {"com.example.app"};
  config.hidden_tokens = {"lineage"};
  yukari::ServiceFilter filter(config);

  assert(filter.matchesCaller(10234, "com.example.app"));
  yukari::FilterConfig multi_user_config = config;
  multi_user_config.target_uids = {1010234};
  yukari::ServiceFilter multi_user_filter(multi_user_config);
  assert(multi_user_filter.matchesCaller(1010234, "com.example.app"));
  assert(!multi_user_filter.matchesCaller(10234, "com.example.app"));
  yukari::FilterConfig core_config = config;
  core_config.target_uids = {1000, 101000, 102000, 109997};
  yukari::ServiceFilter core_filter(core_config);
  assert(!core_filter.matchesCaller(1000, "com.example.app"));
  assert(!core_filter.matchesCaller(101000, "com.example.app"));
  assert(!core_filter.matchesCaller(102000, "com.example.app"));
  assert(!core_filter.matchesCaller(109997, "com.example.app"));
  assert(!filter.matchesCaller(1000, "com.example.app"));
  assert(!filter.matchesCaller(2000, "com.example.app"));
  assert(!filter.matchesCaller(9997, "com.example.app"));
  assert(!filter.matchesCaller(10235, "com.example.app"));
  assert(!filter.matchesCaller(10234, "system_server"));
  assert(filter.shouldHide("vendor.lineage.power"));
  assert(!filter.shouldHide("android.hardware.camera"));
  const std::string input = "android.hardware.camera: [x]\n"
                            "vendor.lineage.power: [x]\n"
                            "malformed output\n";
  assert(filter.filterServiceList(input) ==
         "android.hardware.camera: [x]\nmalformed output\n");

  const std::string indexed =
      "Found 3 services:\n"
      "0    android.hardware.camera: [x]\n"
      "1    vendor.lineage.power: [x]\n"
      "2    android.hardware.audio: [x]\n";
  assert(filter.filterServiceList(indexed) ==
         "Found 2 services:\n"
         "0    android.hardware.camera: [x]\n"
         "1    android.hardware.audio: [x]\n");

  const std::string mixed =
      "Found 3 services:\r\n"
      "0    android.hardware.camera: [x]\r\n"
      "garbled diagnostic\r\n"
      "1    vendor.lineage.power: [x]\r\n"
      "2    android.hardware.audio: [x]";
  assert(filter.filterServiceList(mixed) ==
         "Found 2 services:\r\n"
         "0    android.hardware.camera: [x]\r\n"
         "garbled diagnostic\r\n"
         "1    android.hardware.audio: [x]");

  const std::string malformed =
      "Found services:\n"
      "vendor.lineage.power: [x]\n"
      "android.hardware.camera: [x]\n";
  assert(filter.filterServiceList(malformed) ==
         "Found services:\n"
         "android.hardware.camera: [x]\n");
  assert(filter.shouldHide("malformed lineage output"));
  return 0;
}

