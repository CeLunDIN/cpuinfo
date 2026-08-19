/* Copyright (c) 2026 LoongArch TensorFlow Lite porting project. */

#include <cpuinfo/internal-api.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

static void read_cache(uint32_t index, struct cpuinfo_cache* cache) {
  char path[128];
  snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu0/cache/index%u/size", index);
  cache->size = read_uint(path, 0);
  snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu0/cache/index%u/number_of_sets", index);
  cache->sets = read_uint(path, 0);
  snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu0/cache/index%u/ways_of_associativity", index);
  cache->associativity = read_uint(path, 0);
  snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu0/cache/index%u/coherency_line_size", index);
  cache->line_size = read_uint(path, 0);
  snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu0/cache/index%u/level", index);
  cache->processor_start = read_uint(path, 0);
  cache->processor_count = 1;
  cache->partitions = 1;
  cache->flags = 0;
}

void cpuinfo_loongarch_linux_init(void) {
  const uint32_t processors_count = (uint32_t)sysconf(_SC_NPROCESSORS_ONLN);
  if (processors_count == 0) return;

  struct cpuinfo_processor* processors = calloc(processors_count, sizeof(*processors));
  struct cpuinfo_core* cores = calloc(processors_count, sizeof(*cores));
  struct cpuinfo_cluster* clusters = calloc(1, sizeof(*clusters));
  struct cpuinfo_package* packages = calloc(1, sizeof(*packages));
  struct cpuinfo_uarch_info* uarchs = calloc(1, sizeof(*uarchs));
  if (processors == NULL || cores == NULL || clusters == NULL ||
      packages == NULL || uarchs == NULL) {
    free(processors); free(cores); free(clusters); free(packages); free(uarchs);
    return;
  }

  read_model_name(packages[0].name);
  packages[0].processor_count = processors_count;
  packages[0].core_count = processors_count;
  packages[0].cluster_count = 1;

  clusters[0].processor_count = processors_count;
  clusters[0].core_count = processors_count;
  clusters[0].package = &packages[0];
    clusters[0].uarch = cpuinfo_uarch_loongson_3a6000;
    clusters[0].vendor = cpuinfo_vendor_loongson;

  for (uint32_t i = 0; i < processors_count; i++) {
    char path[128];
    snprintf(path, sizeof(path),
      "/sys/devices/system/cpu/cpu%u/topology/core_id", i);
    cores[i].processor_start = i;
    cores[i].processor_count = 1;
    cores[i].core_id = read_uint(path, i);
    cores[i].cluster = &clusters[0];
    cores[i].package = &packages[0];
    cores[i].uarch = cpuinfo_uarch_loongson_3a6000;
    cores[i].vendor = cpuinfo_vendor_loongson;

    processors[i].linux_id = (int)i;
    processors[i].smt_id = 0;
    processors[i].core = &cores[i];
    processors[i].cluster = &clusters[0];
    processors[i].package = &packages[0];
  }

  uarchs[0].uarch = cpuinfo_uarch_loongson_3a6000;
  uarchs[0].processor_count = processors_count;
  uarchs[0].core_count = processors_count;

  cpuinfo_processors = processors;
  cpuinfo_processors_count = processors_count;
  cpuinfo_cores = cores;
  cpuinfo_cores_count = processors_count;
  cpuinfo_clusters = clusters;
  cpuinfo_clusters_count = 1;
  cpuinfo_packages = packages;
  cpuinfo_packages_count = 1;
  cpuinfo_uarchs = uarchs;
  cpuinfo_uarchs_count = 1;
  cpuinfo_is_initialized = true;
}
