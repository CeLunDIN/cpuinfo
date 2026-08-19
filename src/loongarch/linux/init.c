/* Copyright (c) 2026 LoongArch TensorFlow Lite porting project. */

#include <cpuinfo/internal-api.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <unistd.h>

#ifndef HWCAP_LOONGARCH_LSX
#define HWCAP_LOONGARCH_LSX (1UL << 4)
#endif
#ifndef HWCAP_LOONGARCH_LASX
#define HWCAP_LOONGARCH_LASX (1UL << 5)
#endif
#define CACHE_INDEX_MAX 8

static uint32_t read_uint(const char* path, uint32_t fallback) {
	FILE* file = fopen(path, "r");
	unsigned long value = 0;
	if (file == NULL || fscanf(file, "%lu", &value) != 1) {
		if (file != NULL) fclose(file);
		return fallback;
	}
	fclose(file);
	return (uint32_t)value;
}

static uint32_t read_size(const char* path) {
	FILE* file = fopen(path, "r");
	unsigned long value = 0;
	char suffix = 0;
	if (file == NULL || fscanf(file, "%lu%c", &value, &suffix) < 1) {
		if (file != NULL) fclose(file);
		return 0;
	}
	fclose(file);
	if (suffix == 'K' || suffix == 'k') value *= 1024;
	else if (suffix == 'M' || suffix == 'm') value *= 1024 * 1024;
	return (uint32_t)value;
}

static void read_model_name(char name[CPUINFO_PACKAGE_NAME_MAX]) {
	FILE* file = fopen("/proc/cpuinfo", "r");
	char line[128];
	name[0] = '\0';
	if (file == NULL) return;
	while (fgets(line, sizeof(line), file) != NULL) {
		if (strncmp(line, "model name", 10) == 0) {
			char* value = strchr(line, ':');
			if (value != NULL) {
				value++;
				while (*value == ' ' || *value == '\t') value++;
				strncpy(name, value, CPUINFO_PACKAGE_NAME_MAX - 1);
				name[CPUINFO_PACKAGE_NAME_MAX - 1] = '\0';
				name[strcspn(name, "\r\n")] = '\0';
			}
			break;
		}
	}
	fclose(file);
}

static uint32_t read_topology_id(uint32_t cpu, const char* name, uint32_t fallback) {
	char path[160];
	snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%u/topology/%s", cpu, name);
	return read_uint(path, fallback);
}

static uint32_t shared_cpu_count(uint32_t cpu, uint32_t index) {
	char path[192], list[256];
	FILE* file;
	snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%u/cache/index%u/shared_cpu_list", cpu, index);
	file = fopen(path, "r");
	if (file == NULL || fgets(list, sizeof(list), file) == NULL) {
		if (file != NULL) fclose(file);
		return 1;
	}
	fclose(file);
	uint32_t count = 0;
	char* cursor = list;
	while (*cursor != '\0') {
		char* end = NULL;
		unsigned long first = strtoul(cursor, &end, 10);
		if (end == cursor) break;
		unsigned long last = first;
		if (*end == '-') last = strtoul(end + 1, &end, 10);
		if (last >= first) count += (uint32_t)(last - first + 1);
		while (*end != '\0' && *end != ',') end++;
		if (*end == ',') end++;
		cursor = end;
	}
	return count == 0 ? 1 : count;
}

static bool cache_shares_cpu(uint32_t owner, uint32_t index, uint32_t cpu) {
	char path[192], list[256];
	FILE* file;
	snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%u/cache/index%u/shared_cpu_list", owner, index);
	file = fopen(path, "r");
	if (file == NULL || fgets(list, sizeof(list), file) == NULL) {
		if (file != NULL) fclose(file);
		return cpu == owner;
	}
	fclose(file);
	char* cursor = list;
	while (*cursor != '\0') {
		char* end = NULL;
		unsigned long first = strtoul(cursor, &end, 10);
		if (end == cursor) break;
		unsigned long last = first;
		if (*end == '-') last = strtoul(end + 1, &end, 10);
		if (cpu >= first && cpu <= last) return true;
		while (*end != '\0' && *end != ',') end++;
		if (*end == ',') end++;
		cursor = end;
	}
	return false;
}

static struct cpuinfo_cache* get_cache(struct cpuinfo_cache* caches, uint32_t* count,
		uint32_t capacity, uint32_t cpu, uint32_t index, uint32_t level, bool instruction) {
	char path[192], type[32];
	FILE* file;
	snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%u/cache/index%u/type", cpu, index);
	file = fopen(path, "r");
	if (file == NULL || fgets(type, sizeof(type), file) == NULL) {
		if (file != NULL) fclose(file);
		return NULL;
	}
	fclose(file);
	type[strcspn(type, "\r\n")] = '\0';
	if ((strcmp(type, "Instruction") == 0) != instruction) return NULL;
	snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%u/cache/index%u/size", cpu, index);
	uint32_t size = read_size(path);
	if (size == 0) return NULL;
	uint32_t shared_count = shared_cpu_count(cpu, index);
	uint32_t shared_start = cpu;
	for (uint32_t candidate = 0; candidate < cpu + 1; candidate++) {
		if (cache_shares_cpu(cpu, index, candidate)) { shared_start = candidate; break; }
	}
	for (uint32_t i = 0; i < *count; i++) {
		if (caches[i].size == size && caches[i].processor_start == shared_start &&
			caches[i].processor_count == shared_count) return &caches[i];
	}
	if (*count == capacity) return NULL;
	struct cpuinfo_cache* cache = &caches[(*count)++];
	snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%u/cache/index%u/number_of_sets", cpu, index);
	cache->sets = read_uint(path, 0);
	snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%u/cache/index%u/ways_of_associativity", cpu, index);
	cache->associativity = read_uint(path, 0);
	snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%u/cache/index%u/coherency_line_size", cpu, index);
	cache->line_size = read_uint(path, 0);
	cache->size = size;
	cache->partitions = 1;
	cache->flags = (!instruction && level >= 2) ? CPUINFO_CACHE_UNIFIED : 0;
	cache->processor_start = shared_start;
	cache->processor_count = shared_count;
	return cache;
}

void cpuinfo_loongarch_linux_init(void) {
	const uint32_t processors_count = (uint32_t)sysconf(_SC_NPROCESSORS_ONLN);
	if (processors_count == 0) return;
	cpuinfo_isa.lsx = (getauxval(AT_HWCAP) & HWCAP_LOONGARCH_LSX) != 0;
	cpuinfo_isa.lasx = (getauxval(AT_HWCAP) & HWCAP_LOONGARCH_LASX) != 0;
	struct cpuinfo_processor* processors = calloc(processors_count, sizeof(*processors));
	struct cpuinfo_core* cores = calloc(processors_count, sizeof(*cores));
	struct cpuinfo_cluster* clusters = calloc(processors_count, sizeof(*clusters));
	struct cpuinfo_package* packages = calloc(processors_count, sizeof(*packages));
	struct cpuinfo_uarch_info* uarchs = calloc(1, sizeof(*uarchs));
	struct cpuinfo_cache* caches[cpuinfo_cache_level_max] = {NULL};
	uint32_t cache_counts[cpuinfo_cache_level_max] = {0};
	uint32_t package_ids[processors_count];
	for (uint32_t level = 0; level < cpuinfo_cache_level_max; level++) caches[level] = calloc(processors_count * CACHE_INDEX_MAX, sizeof(struct cpuinfo_cache));
	if (processors == NULL || cores == NULL || clusters == NULL || packages == NULL || uarchs == NULL) return;
	uint32_t core_count = 0, cluster_count = 0, package_count = 0;
	for (uint32_t cpu = 0; cpu < processors_count; cpu++) {
		uint32_t package_id = read_topology_id(cpu, "physical_package_id", 0);
		uint32_t cluster_id = read_topology_id(cpu, "cluster_id", package_id);
		uint32_t core_id = read_topology_id(cpu, "core_id", cpu);
		uint32_t package_index = UINT32_MAX, cluster_index = UINT32_MAX, core_index = UINT32_MAX;
		for (uint32_t i = 0; i < package_count; i++) if (package_ids[i] == package_id) package_index = i;
		if (package_index == UINT32_MAX) { package_index = package_count++; package_ids[package_index] = package_id; packages[package_index].processor_start = cpu; packages[package_index].core_start = core_count; packages[package_index].cluster_start = cluster_count; packages[package_index].cluster_count = 0; }
		for (uint32_t i = 0; i < cluster_count; i++) if (clusters[i].cluster_id == cluster_id && clusters[i].package == &packages[package_index]) cluster_index = i;
		if (cluster_index == UINT32_MAX) { cluster_index = cluster_count++; clusters[cluster_index].cluster_id = cluster_id; clusters[cluster_index].package = &packages[package_index]; clusters[cluster_index].processor_start = cpu; clusters[cluster_index].core_start = core_count; packages[package_index].cluster_count++; }
		for (uint32_t i = 0; i < core_count; i++) if (cores[i].core_id == core_id && cores[i].package == &packages[package_index]) core_index = i;
		if (core_index == UINT32_MAX) { core_index = core_count++; cores[core_index].core_id = core_id; cores[core_index].package = &packages[package_index]; cores[core_index].cluster = &clusters[cluster_index]; cores[core_index].processor_start = cpu; }
		processors[cpu].linux_id = (int)cpu;
		processors[cpu].smt_id = cpu - cores[core_index].processor_start;
		processors[cpu].core = &cores[core_index]; processors[cpu].cluster = &clusters[cluster_index]; processors[cpu].package = &packages[package_index];
		cores[core_index].processor_count++; clusters[cluster_index].processor_count++; packages[package_index].processor_count++;
		for (uint32_t index = 0; index < CACHE_INDEX_MAX; index++) {
			char path[192];
			snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%u/cache/index%u/level", cpu, index);
			uint32_t level = read_uint(path, 0);
			if (level == 0 || level >= cpuinfo_cache_level_max) break;
			bool instruction = false;
			snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%u/cache/index%u/type", cpu, index);
			FILE* type_file = fopen(path, "r"); char type[32] = {0};
			if (type_file != NULL) { fgets(type, sizeof(type), type_file); fclose(type_file); instruction = strncmp(type, "Instruction", 11) == 0; }
			const uint32_t cache_slot = level == 1 ? (instruction ? cpuinfo_cache_level_1i : cpuinfo_cache_level_1d) : level;
			struct cpuinfo_cache* cache = get_cache(caches[cache_slot], &cache_counts[cache_slot], processors_count * CACHE_INDEX_MAX, cpu, index, level, instruction);
			if (cache == NULL) continue;
			if (level == 1) { if (instruction) processors[cpu].cache.l1i = cache; else processors[cpu].cache.l1d = cache; }
			else if (level == 2) processors[cpu].cache.l2 = cache;
			else if (level == 3) processors[cpu].cache.l3 = cache;
		}
	}
	read_model_name(packages[0].name);
	for (uint32_t i = 0; i < package_count; i++) { packages[i].core_count = (i + 1 < package_count ? packages[i + 1].core_start : core_count) - packages[i].core_start; }
	for (uint32_t i = 0; i < cluster_count; i++) { clusters[i].core_count = (i + 1 < cluster_count ? clusters[i + 1].core_start : core_count) - clusters[i].core_start; clusters[i].vendor = cpuinfo_vendor_loongson; clusters[i].uarch = cpuinfo_uarch_loongson_3a6000; }
	for (uint32_t i = 0; i < core_count; i++) { cores[i].vendor = cpuinfo_vendor_loongson; cores[i].uarch = cpuinfo_uarch_loongson_3a6000; }
	uarchs[0] = (struct cpuinfo_uarch_info){cpuinfo_uarch_loongson_3a6000, processors_count, core_count};
	cpuinfo_processors = processors; cpuinfo_processors_count = processors_count; cpuinfo_cores = cores; cpuinfo_cores_count = core_count; cpuinfo_clusters = clusters; cpuinfo_clusters_count = cluster_count; cpuinfo_packages = packages; cpuinfo_packages_count = package_count; cpuinfo_uarchs = uarchs; cpuinfo_uarchs_count = 1;
	for (uint32_t level = 0; level < cpuinfo_cache_level_max; level++) { cpuinfo_cache[level] = caches[level]; cpuinfo_cache_count[level] = cache_counts[level]; }
	cpuinfo_max_cache_size = cpuinfo_compute_max_cache_size(&processors[0]);
	cpuinfo_is_initialized = true;
}
