/* Copyright 2016 Stanford University
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "stencil_mapper.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <map>
#include <vector>

#include "default_mapper.h"

using namespace LegionRuntime::HighLevel;

///
/// Mapper
///

static LegionRuntime::Logger::Category log_stencil("stencil");

class StencilMapper : public DefaultMapper
{
public:
  StencilMapper(Machine machine, HighLevelRuntime *rt, Processor local,
                std::vector<Processor>* procs_list,
                std::vector<Memory>* sysmems_list,
                std::map<Memory, std::vector<Processor> >* sysmem_local_procs,
                std::map<Processor, Memory>* proc_sysmems,
                std::map<Processor, Memory>* proc_regmems);
  virtual void select_task_options(Task *task);
  virtual void select_task_variant(Task *task);
  virtual bool map_task(Task *task);
  virtual bool map_inline(Inline *inline_operation);
  virtual bool map_copy(Copy *copy);
  virtual void notify_mapping_failed(const Mappable *mappable);
  virtual bool map_must_epoch(const std::vector<Task*> &tasks,
                              const std::vector<MappingConstraint> &constraints,
                              MappingTagID tag);
private:
  Color get_task_color_by_region(Task *task, const RegionRequirement &requirement);
  LogicalRegion get_root_region(LogicalRegion handle);
  LogicalRegion get_root_region(LogicalPartition handle);
private:
  std::vector<Processor>& procs_list;
  std::vector<Memory>& sysmems_list;
  std::map<Memory, std::vector<Processor> >& sysmem_local_procs;
  std::map<Processor, Memory>& proc_sysmems;
  std::map<Processor, Memory>& proc_regmems;
};

StencilMapper::StencilMapper(Machine machine, HighLevelRuntime *rt, Processor local,
                            std::vector<Processor>* _procs_list,
                            std::vector<Memory>* _sysmems_list,
                            std::map<Memory, std::vector<Processor> >* _sysmem_local_procs,
                            std::map<Processor, Memory>* _proc_sysmems,
                            std::map<Processor, Memory>* _proc_regmems)
  : DefaultMapper(machine, rt, local),
    procs_list(*_procs_list),
    sysmems_list(*_sysmems_list),
    sysmem_local_procs(*_sysmem_local_procs),
    proc_sysmems(*_proc_sysmems),
    proc_regmems(*_proc_regmems)
{
}

void StencilMapper::select_task_options(Task *task)
{
  // Task options:
  task->inline_task = false;
  task->spawn_task = false;
  task->map_locally = true;
  task->profile_task = false;
  const char* task_name = task->get_task_name();
  if ((strcmp(task_name, "st") == 0 ||
       strcmp(task_name, "increment") == 0) &&
      !task->is_index_space)
  {
    std::vector<Processor> &local_procs =
      sysmem_local_procs[proc_sysmems[task->target_proc]];
    Color index = get_logical_region_color(task->regions[0].region);
    task->target_proc = local_procs[(index % (local_procs.size() - 1)) + 1];
    //printf("Task " IDFMT " has color %d mapped to Proc " IDFMT "\n",
    //    task->get_unique_task_id(), index, task->target_proc.id);
  }
}

void StencilMapper::select_task_variant(Task *task)
{
  // Use the SOA variant for all tasks.
  // task->selected_variant = VARIANT_SOA;
  DefaultMapper::select_task_variant(task);

  std::vector<RegionRequirement> &regions = task->regions;
  for (std::vector<RegionRequirement>::iterator it = regions.begin();
        it != regions.end(); it++) {
    RegionRequirement &req = *it;

    // Select SOA layout for all regions.
    req.blocking_factor = req.max_blocking_factor;
  }
}

bool StencilMapper::map_task(Task *task)
{
  Memory sysmem = proc_sysmems[task->target_proc];
  assert(sysmem.exists());
  std::vector<RegionRequirement> &regions = task->regions;
  for (std::vector<RegionRequirement>::iterator it = regions.begin();
        it != regions.end(); it++) {
    RegionRequirement &req = *it;

    // Region options:
    req.virtual_map = false;
    req.enable_WAR_optimization = false;
    req.reduction_list = false;

    // Place all regions in local system memory.
    req.target_ranking.push_back(sysmem);
    std::set<FieldID> fields;
    get_field_space_fields(req.parent.get_field_space(), fields);
    req.additional_fields.insert(fields.begin(), fields.end());
  }

  return false;
}

bool StencilMapper::map_copy(Copy *copy)
{
  if (strcmp(copy->parent_task->get_task_name(), "main") == 0)
  {
    for (unsigned idx = 0; idx < copy->src_requirements.size(); ++idx)
    {
      RegionRequirement& src_req = copy->src_requirements[idx];
      RegionRequirement& dst_req = copy->dst_requirements[idx];
      Color index = get_logical_region_color(src_req.region);
      Processor target_proc = procs_list[index % procs_list.size()];
      Memory mem = proc_sysmems[target_proc];
      if (dst_req.privilege_fields.size() == 1)
        mem = proc_regmems[target_proc];
      src_req.target_ranking.clear();
      src_req.target_ranking.push_back(proc_sysmems[target_proc]);
      dst_req.target_ranking.clear();
      // dst_req.target_ranking.push_back(mem);
      dst_req.target_ranking.push_back(proc_sysmems[target_proc]);
      src_req.blocking_factor = src_req.max_blocking_factor;
      dst_req.blocking_factor = dst_req.max_blocking_factor;
    }
    return false;
  }
  else {
    return DefaultMapper::map_copy(copy);
  }
}

bool StencilMapper::map_inline(Inline *inline_operation)
{
  Memory sysmem = proc_sysmems[local_proc];
  RegionRequirement &req = inline_operation->requirement;

  // Region options:
  req.virtual_map = false;
  req.enable_WAR_optimization = false;
  req.reduction_list = false;
  req.blocking_factor = req.max_blocking_factor;

  // Place all regions in global memory.
  req.target_ranking.push_back(sysmem);

  log_stencil.debug(
    "inline mapping region (%d,%d,%d) target ranking front " IDFMT " (size %zu)",
    req.region.get_index_space().get_id(),
    req.region.get_field_space().get_id(),
    req.region.get_tree_id(),
    req.target_ranking[0].id,
    req.target_ranking.size());

  return false;
}

bool StencilMapper::map_must_epoch(const std::vector<Task*> &tasks,
                                    const std::vector<MappingConstraint> &constraints,
                                    MappingTagID tag)
{
  unsigned tasks_per_sysmem = (tasks.size() + sysmems_list.size() - 1) / sysmems_list.size();
  for (unsigned i = 0; i < tasks.size(); ++i)
  {
    Task* task = tasks[i];
    unsigned index = task->index_point.point_data[0];
    assert(index / tasks_per_sysmem < sysmems_list.size());
    Memory sysmem = sysmems_list[index / tasks_per_sysmem];
    unsigned subindex = index % tasks_per_sysmem;
    assert(subindex < sysmem_local_procs[sysmem].size());
    task->target_proc = sysmem_local_procs[sysmem][subindex];
    map_task(task);
    task->additional_procs.clear();
  }

  typedef std::map<LogicalRegion, Memory> Mapping;
  Mapping mappings;
  for (unsigned i = 0; i < constraints.size(); ++i)
  {
    const MappingConstraint& c = constraints[i];
    if (c.t1->regions[c.idx1].flags & NO_ACCESS_FLAG &&
        c.t2->regions[c.idx2].flags & NO_ACCESS_FLAG)
      continue;

    Memory regmem;
    if (c.t2->regions[c.idx2].flags & NO_ACCESS_FLAG)
      regmem = proc_sysmems[c.t1->target_proc]; // proc_regmems[c.t1->target_proc];
    else if (c.t1->regions[c.idx1].flags & NO_ACCESS_FLAG)
      regmem = proc_sysmems[c.t2->target_proc]; // proc_regmems[c.t2->target_proc];
    else
      assert(0);
    c.t1->regions[c.idx1].target_ranking.clear();
    c.t1->regions[c.idx1].target_ranking.push_back(regmem);
    c.t2->regions[c.idx2].target_ranking.clear();
    c.t2->regions[c.idx2].target_ranking.push_back(regmem);
    mappings[c.t1->regions[c.idx1].region] = regmem;
  }

  for (unsigned i = 0; i < constraints.size(); ++i)
  {
    const MappingConstraint& c = constraints[i];
    if (c.t1->regions[c.idx1].flags & NO_ACCESS_FLAG &&
        c.t2->regions[c.idx2].flags & NO_ACCESS_FLAG)
    {
      Mapping::iterator it =
        mappings.find(c.t1->regions[c.idx1].region);
      assert(it != mappings.end());
      Memory regmem = it->second;
      c.t1->regions[c.idx1].target_ranking.clear();
      c.t1->regions[c.idx1].target_ranking.push_back(regmem);
      c.t2->regions[c.idx2].target_ranking.clear();
      c.t2->regions[c.idx2].target_ranking.push_back(regmem);
    }
  }

  return false;
}

void StencilMapper::notify_mapping_failed(const Mappable *mappable)
{
  switch (mappable->get_mappable_kind()) {
  case Mappable::TASK_MAPPABLE:
    {
      log_stencil.warning("mapping failed on task");
      break;
    }
  case Mappable::COPY_MAPPABLE:
    {
      log_stencil.warning("mapping failed on copy");
      break;
    }
  case Mappable::INLINE_MAPPABLE:
    {
      Inline *_inline = mappable->as_mappable_inline();
      RegionRequirement &req = _inline->requirement;
      LogicalRegion region = req.region;
      log_stencil.warning(
        "mapping %s on inline region (%d,%d,%d) memory " IDFMT,
        (req.mapping_failed ? "failed" : "succeeded"),
        region.get_index_space().get_id(),
        region.get_field_space().get_id(),
        region.get_tree_id(),
        req.selected_memory.id);
      break;
    }
  case Mappable::ACQUIRE_MAPPABLE:
    {
      log_stencil.warning("mapping failed on acquire");
      break;
    }
  case Mappable::RELEASE_MAPPABLE:
    {
      log_stencil.warning("mapping failed on release");
      break;
    }
  }
  assert(0 && "mapping failed");
}

Color StencilMapper::get_task_color_by_region(Task *task, const RegionRequirement &requirement)
{
  if (requirement.handle_type == SINGULAR) {
    return get_logical_region_color(requirement.region);
  }
  return 0;
}

LogicalRegion StencilMapper::get_root_region(LogicalRegion handle)
{
  if (has_parent_logical_partition(handle)) {
    return get_root_region(get_parent_logical_partition(handle));
  }
  return handle;
}

LogicalRegion StencilMapper::get_root_region(LogicalPartition handle)
{
  return get_root_region(get_parent_logical_region(handle));
}

static void create_mappers(Machine machine, HighLevelRuntime *runtime, const std::set<Processor> &local_procs)
{
  printf("starting create_mappers absolute time %llu\n", Realm::Clock::current_time_in_microseconds());
  fflush(stdout);

  std::vector<Processor>* procs_list = new std::vector<Processor>();
  std::vector<Memory>* sysmems_list = new std::vector<Memory>();
  std::map<Memory, std::vector<Processor> >* sysmem_local_procs =
    new std::map<Memory, std::vector<Processor> >();
  std::map<Processor, Memory>* proc_sysmems = new std::map<Processor, Memory>();
  std::map<Processor, Memory>* proc_regmems = new std::map<Processor, Memory>();


  std::vector<Machine::ProcessorMemoryAffinity> proc_mem_affinities;
  machine.get_proc_mem_affinity(proc_mem_affinities);

  for (unsigned idx = 0; idx < proc_mem_affinities.size(); ++idx) {
    Machine::ProcessorMemoryAffinity& affinity = proc_mem_affinities[idx];
    if (affinity.p.kind() == Processor::LOC_PROC) {
      if (affinity.m.kind() == Memory::SYSTEM_MEM) {
        (*proc_sysmems)[affinity.p] = affinity.m;
        if (proc_regmems->find(affinity.p) == proc_regmems->end())
          (*proc_regmems)[affinity.p] = affinity.m;
      }
      else if (affinity.m.kind() == Memory::REGDMA_MEM)
        (*proc_regmems)[affinity.p] = affinity.m;
    }
  }

  for (std::map<Processor, Memory>::iterator it = proc_sysmems->begin();
       it != proc_sysmems->end(); ++it) {
    procs_list->push_back(it->first);
    (*sysmem_local_procs)[it->second].push_back(it->first);
  }

  for (std::map<Memory, std::vector<Processor> >::iterator it =
        sysmem_local_procs->begin(); it != sysmem_local_procs->end(); ++it)
    sysmems_list->push_back(it->first);

  for (std::set<Processor>::const_iterator it = local_procs.begin();
        it != local_procs.end(); it++)
  {
    StencilMapper* mapper = new StencilMapper(machine, runtime, *it,
                                              procs_list,
                                              sysmems_list,
                                              sysmem_local_procs,
                                              proc_sysmems,
                                              proc_regmems);
    runtime->replace_default_mapper(mapper, *it);
  }

  printf("finished create_mappers absolute time %llu\n", Realm::Clock::current_time_in_microseconds());
  fflush(stdout);
}

void register_mappers()
{
  HighLevelRuntime::set_registration_callback(create_mappers);
}
