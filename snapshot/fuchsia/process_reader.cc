// Copyright 2018 The Crashpad Authors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "snapshot/fuchsia/process_reader.h"

#include <link.h>
#include <zircon/syscalls.h>

namespace crashpad {

ProcessReader::Module::Module() = default;

ProcessReader::Module::~Module() = default;

ProcessReader::ProcessReader() = default;

ProcessReader::~ProcessReader() = default;

bool ProcessReader::Initialize(zx_handle_t process) {
  INITIALIZATION_STATE_SET_INITIALIZING(initialized_);

  process_ = process;

  process_memory_.reset(new ProcessMemoryFuchsia());
  process_memory_->Initialize(process_);

  INITIALIZATION_STATE_SET_VALID(initialized_);
  return true;
}

const std::vector<ProcessReader::Module>& ProcessReader::Modules() {
  INITIALIZATION_STATE_DCHECK_VALID(initialized_);

  if (!initialized_modules_) {
    InitializeModules();
  }

  return modules_;
}

void ProcessReader::InitializeModules() {
  DCHECK(!initialized_modules_);
  DCHECK(modules_.empty());

  initialized_modules_ = true;

  // TODO(scottmg): <inspector/inspector.h> does some of this, but doesn't
  // expose any of the data that's necessary to fill out a Module after it
  // retrieves (some of) the data into internal structures. It may be worth
  // trying to refactor/upstream some of this into Fuchsia.

  std::string app_name("app:");
  {
    char name[ZX_MAX_NAME_LEN];
    zx_status_t status =
        zx_object_get_property(process_, ZX_PROP_NAME, name, sizeof(name));
    if (status != ZX_OK) {
      LOG(ERROR) << "zx_object_get_property ZX_PROP_NAME";
      return;
    }

    app_name += name;
  }

  // Starting from the ld.so's _dl_debug_addr, read the link_map structure and
  // walk the list to fill out modules_.

  uintptr_t debug_address;
  zx_status_t status = zx_object_get_property(process_,
                                              ZX_PROP_PROCESS_DEBUG_ADDR,
                                              &debug_address,
                                              sizeof(debug_address));
  if (status != ZX_OK || debug_address == 0) {
    LOG(ERROR) << "zx_object_get_property ZX_PROP_PROCESS_DEBUG_ADDR";
    return;
  }

  constexpr auto k_r_debug_map_offset = offsetof(r_debug, r_map);
  uintptr_t map;
  if (!process_memory_->Read(
          debug_address + k_r_debug_map_offset, sizeof(map), &map)) {
    LOG(ERROR) << "read link_map";
    return;
  }

  int i = 0;
  constexpr int kMaxDso = 1000;  // Stop after an unreasonably large number.
  while (map != 0) {
    if (++i >= kMaxDso) {
      LOG(ERROR) << "possibly circular dso list, terminating";
      return;
    }

    constexpr auto k_link_map_addr_offset = offsetof(link_map, l_addr);
    zx_vaddr_t base;
    if (!process_memory_->Read(
            map + k_link_map_addr_offset, sizeof(base), &base)) {
      LOG(ERROR) << "Read base";
      // Could theoretically continue here, but realistically if any part of
      // link_map fails to read, things are looking bad, so just abort.
      break;
    }

    constexpr auto k_link_map_next_offset = offsetof(link_map, l_next);
    zx_vaddr_t next;
    if (!process_memory_->Read(
            map + k_link_map_next_offset, sizeof(next), &next)) {
      LOG(ERROR) << "Read next";
      break;
    }

    constexpr auto k_link_map_name_offset = offsetof(link_map, l_name);
    zx_vaddr_t name_address;
    if (!process_memory_->Read(map + k_link_map_name_offset,
                               sizeof(name_address),
                               &name_address)) {
      LOG(ERROR) << "Read name address";
      break;
    }

    std::string dsoname;
    if (!process_memory_->ReadCString(name_address, &dsoname)) {
      // In this case, it could be reasonable to continue on to the next module
      // as this data isn't strictly in the link_map.
      LOG(ERROR) << "ReadCString name";
    }

    Module module;
    module.name = dsoname.empty() ? app_name : dsoname;

    std::unique_ptr<ElfImageReader> reader(new ElfImageReader());

    std::unique_ptr<ProcessMemoryRange> process_memory_range(
        new ProcessMemoryRange());
    // TODO(scottmg): Could this be limited range?
    process_memory_range->Initialize(process_memory_.get(), true);
    process_memory_ranges_.push_back(std::move(process_memory_range));

    reader->Initialize(*process_memory_ranges_.back(), base);
    module.reader = reader.get();
    module_readers_.push_back(std::move(reader));
    modules_.push_back(module);

    map = next;
  }
}

}  // namespace crashpad
