#define NUM 10
extern pid_t pid_table[NUM], script_table[NUM];
extern char *hp_bench, *be_bench;
int bandwidth_saturation (double val1);
int stable_bandwidth (double val1, double val2);
int stable_ipc (double val1, double val2);
int better_ipc (double val1, double val2);
double transform_MB_to_GB (double val1);
double transform_MB_to_GB (double val1);
void dicer_driver (void);
void thread_pack(void);
void thread_pack_pid(pid_t val1);

int find_ways(void);
int pid_core_num(const pid_t val1);

double calculate_geomean (double *val);

void selfn_benchmarks(const char* val1);
void selfn_cores(const char* val1);
void apply_new_allocation(int m);
void class_update(void);
void dicer_init(void);
void retrieve_the_executables(void);
void start_a_process(pid_t *ptr_pid, pid_t *script_pid, int offset, char* core, char *executable_name);
int best_allocation(double *val);
void allocation_sampling(void);
void allocation_optimisation(void);
int check_instances_and_restart(struct pqos_mon_data **val);
void allocation_reset(void);
void store_the_ipc(double val, int val1, int val2);
void build_header_csv(char *val, const size_t val1);
void free_space(void);
void print_results_to_csv(FILE *val2, char *val3, const pid_t val4, int val1, struct pqos_event_values *val, struct pqos_event_values *val5, int val6);
void store_bw_sat_counter(void);
