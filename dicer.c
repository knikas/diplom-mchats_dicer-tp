#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <ctype.h>                                      /**< isspace() */
#include <sys/types.h>                                  /**< open() */
#include <sys/stat.h>
#include <sys/ioctl.h>                                  /**< terminal ioctl */
#include <sys/time.h>                                   /**< gettimeofday() */
#include <time.h>                                       /**< localtime() */
#include <fcntl.h>
#include <inttypes.h>
#include <sys/wait.h>
#include <stdbool.h>
#include <sched.h>

#include "pqos.h"
#include "main.h"
#include "monitor.h"
#include "alloc.h"
#include "dicer.h"
#include "common.h"

#define PQOS_MAX_PIDS		128
#define PQOS_MON_EVENT_ALL	-1
#define MAX_BUF 		128
#define PQOS_MAX_SOCKETS	8
#define PID_COL_CORE  (39)

/** 
 * BW_LIMITS is measured in GB/sec
 */
#define BW_LIMIT 37.5

#define IPC_SUB_LIMIT 0.95
#define IPC_UPPER_LIMIT 1.05
#define BW_UPPER_LIMIT 1.30

#define NUM_SAMPLES	5
#define SAMPLING_TIME 	200000

/**
 * N_HIS_MEAS is the number of history measurements
 * the scheduler needs to make an allocation decision
 * FSM: Measurement(n), Measurement(n-1)
 */
#define N_HIS_MEAS 4

struct process_info {
	char *benchmark;
	char *core;
	int  status;
	int  restarts;
	uint64_t instructions;
	uint64_t cycles;
};

static int samples[NUM_SAMPLES]={19, 15, 10, 5, 2};

int allocation[N_HIS_MEAS], current_allocation, opt_allocation, stopped_processes = 0, bw_sat_counter = 0, prev_sampling = 0, prev_reset = 0, tp_en = 0, prev_sam_sat = 0, prev_tp = 0;
int high_prio_restarted = 0, max_cores = 9, cores_tmp = 9;
double ipc[N_HIS_MEAS], bw_hp[N_HIS_MEAS], hp_mpki[N_HIS_MEAS], sample_ipc[NUM_SAMPLES], ipc_opt = 0.0, geometric_mean, coeff, coeff_sam;
char *hp_bench, *be_bench, *core_list[NUM];
static char *sel_output_file = NULL;
static int high_prio_core = 0;
pid_t pid_table[NUM], script_table[NUM];
struct process_info *info_table[NUM];
struct pqos_mon_data **mon_data = NULL, **mon_grps = NULL;
bool CT_favoured, BW_saturated;
uint64_t pmask, cmask;
uint64_t reg = (1ULL << 20) - (1ULL);

uint64_t ways[20] = {0x80000, 0xc0000, 0xe0000, 0xf0000, 
		0xf8000, 0xfc000, 0xfe000, 0xff000, 
		0xff800, 0xffc00, 0xffe00, 0xfff00, 
		0xfff80, 0xfffc0, 0xfffe0, 0xffff0, 
		0xffff8, 0xffffc, 0xffffe, 0xfffff};

void class_update(void) {
	const struct pqos_cap *p_cap = NULL;
	const struct pqos_cpuinfo *p_cpu = NULL;
	unsigned sock_count, *sockets = NULL;
	int ret;
	// Retrieves PQoS Capabilities Data
	ret = pqos_cap_get(&p_cap, &p_cpu);
	if (ret != PQOS_RETVAL_OK) {
		printf("Error retrieving PQoS capabilities!\n");
		return;
	}
	sockets = pqos_cpu_get_sockets(p_cpu, &sock_count);
	if (sockets == NULL) {
		printf("Error retrieving CPU socket information!\n");
		return;
	}
	const unsigned hp = 1, be = 2;
	new_allocation(hp, pmask, sockets, 2);
	new_allocation(be, cmask, sockets, 2);
}

void apply_new_allocation(int m){
	pmask = ways[m-1];
	cmask = (~pmask) & reg;
	class_update();
}

int find_ways(void){
	return current_allocation;
}

int bandwidth_saturation(double val){
	if (val >= BW_LIMIT && val < 90.0)
		return 1;
	return 0;
}

int stable_ipc(double cur_val, double prev_val){
	if ( (cur_val < (IPC_SUB_LIMIT * prev_val)) || (cur_val > (IPC_UPPER_LIMIT * prev_val)) )
		return 0;
	return 1;
}

int stable_bandwidth(double cur_val, double prev_val){
	if ( (cur_val > (BW_UPPER_LIMIT * prev_val)) )
		return 0;
	return 1;
}

int better_ipc(double cur_val, double prev_val){
	if (cur_val > (IPC_UPPER_LIMIT * prev_val))
		return 1;
	return 0;
}

static long timeval_to_usec(const struct timeval *tv) {
        return ((long)tv->tv_usec) + ((long)tv->tv_sec * 1000000L);
}

static void usec_to_timeval(struct timeval *tv, const long usec) {
        tv->tv_sec = usec / 1000000L;
        tv->tv_usec = usec % 1000000L;
}

double transform_MB_to_GB (double val){
	return val/1024;
}

static inline double bytes_to_mb(const double bytes)
{
        return bytes / (1024.0 * 1024.0);
}

static inline double bytes_to_kb(const double bytes)
{
        return bytes / 1024.0;
}

double calculate_geomean (double *val){
	int i;
	double prod = 1.0, geomean;
	for (i = 1; i <  N_HIS_MEAS; i++)
		prod = prod * val[i];
	geomean = pow(prod, (1.0/(N_HIS_MEAS-1)));
	return geomean;
}

static long time_to_take_meas(long cur, long prev) {
	if ((cur-prev) > SAMPLING_TIME) {
		return 1;
	}
	return 0;
}

int best_allocation(double *arr_ipc_val){
	double max = arr_ipc_val[0];
	int retval = 0, i;
	for (i = 1; i < NUM_SAMPLES; i++) {
		if (arr_ipc_val[i] > max)
			max = arr_ipc_val[i];
	}
	for (i = 0; i < NUM_SAMPLES; i++) {
		if (arr_ipc_val[i] > 0.98*max)
			retval = i;
	}
	return retval;
}

void store_bw_sat_counter(void) {
        FILE *f = fopen("IPC.txt", "a");
        if (f != NULL)
                fprintf(f, "BW_SAT_TIMES: %d\n",bw_sat_counter);
        else {
                printf("Error opening file for storing ipc measurements\n");
                exit(1);
        }
        fclose(f);
}

void store_the_ipc(double ipc_measurement, int i, int restart){
	FILE *f = fopen("IPC.txt", "a");
	if (f != NULL)
		fprintf(f, "core,%d,ipc,%f, restart,%d\n", i,ipc_measurement, restart);
	else {
		printf("Error opening file for storing ipc measurements\n");
		exit(1);
	}
	fclose(f);
}

void dicer_driver(void){
	const long interval =
                (long)sel_mon_interval * 100000LL;
        struct timeval tv_start, tv_s;
	int dicer_stop = 0, i, core, counter = 0;
	const size_t sz_header = 128;
	char header[sz_header], cb_time[64];
	coeff = 10.0 / (double)sel_mon_interval;
	coeff_sam = 1000000 / SAMPLING_TIME;

	const int iscsv = !strcasecmp(sel_output_type, "csv");
	if (iscsv) {
		build_header_csv(header, sz_header);
		FILE *fd = fopen_check_symlink(sel_output_file, "w+"); 
		fprintf(fd, "%s\n", header);
		fclose(fd);
	}
	struct pqos_mon_data **ptm;

	gettimeofday(&tv_start, NULL);
        tv_s = tv_start;
	
	while (dicer_stop == 0){
		struct timeval tv_e;
		struct tm *today = NULL;
                long usec_start = 0, usec_end = 0, usec_diff = 0;
		double total_bandwidth = 0.0, total_bandwidth_gb = 0.0;
		allocation[0] = current_allocation;
		ptm = monitor();

		cores_tmp = max_cores;
		for(i=0; i<NUM; i++) {
			const struct pqos_event_values *ptr = &ptm[i]->values;

			if (sel_interface == PQOS_INTER_OS) {
				core = pid_core_num(ptm[i]->pids[0]);
				if (core < 0)
					printf("Error retrieving core from PID\n");
			}
			else
				core = *ptm[i]->cores;

			if (core == high_prio_core) {
				/* Take measurements of HP instance */
				ipc[0] = ptr->ipc;
				bw_hp[0] = bytes_to_mb(ptr->mbm_local_delta) * coeff;
				hp_mpki[0] = (1.0 * (unsigned long long)ptr->llc_misses_delta / (uint64_t) ptr->ipc_retired_delta);
			}
			total_bandwidth = total_bandwidth + bytes_to_mb(ptr->mbm_local_delta) * coeff;
		}

		total_bandwidth_gb = transform_MB_to_GB(total_bandwidth);
		geometric_mean = calculate_geomean(bw_hp);
		
		today = localtime(&tv_s.tv_sec);
                if (iscsv) {
                        if (today != NULL)
                                strftime(cb_time, sizeof(cb_time) - 1,"%Y-%m-%d %H:%M:%S", today);
                        else
                                strncpy(cb_time, "error", sizeof(cb_time) - 1);

                        FILE *fd = fopen_check_symlink(sel_output_file, "a");
                        for(i=0;i<NUM;i++) {
                                if (sel_interface == PQOS_INTER_OS) {                                                                                                                                                                                                                core = pid_core_num(ptm[i]->pids[0]);
                                        if (core < 0)                                                                                                                                                                                                                                        printf("Error retrieving core from PID\n");
                                        print_results_to_csv(fd, cb_time, ptm[i]->pids[0], core, &ptm[i]->values, &ptm[high_prio_core]->values, i);
                                }
                                else {
                                        core = *ptm[i]->cores;
                                        print_results_to_csv(fd, cb_time, 0, core, &ptm[i]->values, &ptm[high_prio_core]->values, i);
                                }
                        }
                        fclose(fd);
                }

		if (bandwidth_saturation(total_bandwidth_gb)) {
			BW_saturated = true;
			bw_sat_counter++;
			if (prev_sam_sat || tp_en) {
				printf("\nDICER driver: Bandwidth Saturated -> Thread Packing & Allocation Sampling\n");
				max_cores--;
				thread_pack();
				prev_sam_sat = 0;
				tp_en = 1;
				prev_tp = 1;
			}
			else {
				printf("\nDICER driver: Bandwidth Saturated -> Allocation Sampling\n");
				allocation_sampling();
				prev_sam_sat = 1;
			}
		}
		else {
			printf("\nDICER driver: Bandwidth OK -> Allocation Optimisation\n");
			if (prev_sam_sat > 0)
				prev_sam_sat--;

			if (prev_tp) {
				allocation_sampling();
				prev_tp = 0;
			}
			else
				allocation_optimisation();
		}
		
		counter++;
		printf("\n=================================%d sec==================================================\n", counter);
		printf("High Priority Bandwidth: %f (MB/sec)\n", bw_hp[0]);
		printf("CT_Favoured: %s, BW_Saturated: %s, Optimal_Allocation : %d, IPC_opt : %f\n",(CT_favoured)? "True" : "False", (BW_saturated)? "True" : "False", opt_allocation, ipc_opt);
		printf("Total Bandwidth Measured: %f (GB/sec)\n", total_bandwidth_gb);
		printf("%f < ipc[0] < %f\n", ipc[1] * IPC_SUB_LIMIT, ipc[1] * IPC_UPPER_LIMIT);
		printf("bw[0] < %f\n", geometric_mean * BW_UPPER_LIMIT);
		printf("ipc[0]: %f, bw_hp[0]: %f (MB/sec), hp_mpki: %f, allocation[0]: %d\n", ipc[0], bw_hp[0], hp_mpki[0], allocation[0]);
		printf("ipc[1]: %f, bw_hp[1]: %f (MB/sec), hp_mpki: %f, allocation[1]: %d\n", ipc[1], bw_hp[1], hp_mpki[1], allocation[1]);
		printf("ipc[2]: %f, bw_hp[2]: %f (MB/sec), hp_mpki: %f, allocation[2]: %d\n", ipc[2], bw_hp[2], hp_mpki[2], allocation[2]);
		printf("ipc[3]: %f, bw_hp[3]: %f (MB/sec), hp_mpki: %f, allocation[3]: %d\n", ipc[3], bw_hp[3], hp_mpki[3], allocation[3]);
		printf("Current_allocation: %d - 0x%" PRIx64 "\n", current_allocation, pmask);
		printf("=========================================================================================\n");
		printf("HP benchmark: %s - BE benchmarks: %s\n",hp_bench,be_bench);
		if (sel_interface == PQOS_INTER_OS) {
			printf("Monitored PIDs:");
			for(i=0;i<NUM;i++)
				printf(" %d",pid_table[i]);
		}
		printf("\nTP Enabled: %d - Max cores: %d\n\n\n",tp_en,max_cores);

		for(i=N_HIS_MEAS-1; i>0; i--) {
			allocation[i] = allocation[i-1];
			ipc[i] = ipc[i-1];
			bw_hp[i] = bw_hp[i-1];
			hp_mpki[i] = hp_mpki[i-1];
		}

		gettimeofday(&tv_e, NULL);
		if (prev_sampling || prev_reset) {
			usec_start = timeval_to_usec(&tv_e);
			prev_sampling = 0;
			prev_reset = 0;
		}
		else
                	usec_start = timeval_to_usec(&tv_s);
                usec_end = timeval_to_usec(&tv_e);
                usec_diff = usec_end - usec_start;
                while (usec_diff < interval && usec_diff >= 0) {
                        dicer_stop = check_instances_and_restart(ptm);
                        if (dicer_stop)
                                break;
                        gettimeofday(&tv_e, NULL);
                        usec_diff = timeval_to_usec(&tv_e) - usec_start;
                }

                /* move tv_s to the next interval */
                usec_to_timeval(&tv_s, usec_start + interval);
		high_prio_restarted = 0;
	}
	free_space();	
}

void thread_pack(void) {
        int i;
        cpu_set_t my_set;
	
	if (max_cores < 9) {
		CPU_ZERO(&my_set);
        	for(i=1; i <= max_cores; i++)
                	CPU_SET(i, &my_set);
        	for(i=1; i<NUM; i++)
                	sched_setaffinity(pid_table[i], sizeof(cpu_set_t), &my_set);
	}
	else {
		for(i=1; i<NUM; i++) {
			CPU_ZERO(&my_set);
			CPU_SET(i, &my_set);
			sched_setaffinity(pid_table[i], sizeof(cpu_set_t), &my_set);
		}
	}
        printf("\nSet mask for all BEs in %d cores\n",max_cores);
}

void thread_pack_pid(pid_t pid) {
        int i;
        cpu_set_t my_set;
        CPU_ZERO(&my_set);

        for(i=1; i <= max_cores; i++)
                CPU_SET(i, &my_set);
        sched_setaffinity(pid, sizeof(cpu_set_t), &my_set);
        printf("\nSet mask for PID %d in %d cores\n",pid,max_cores);
}

void selfn_benchmarks(const char *arg) {
        char *token;
        const char* s = " ";
        char* copy = strdup(arg);

        token = strtok(copy, s);
        hp_bench = strdup(token);
        token = strtok(NULL, s);
        be_bench = strdup(token);
        printf("HP benchmark is: %s\n", hp_bench);
        printf("BE benchmark is: %s\n", be_bench);
}

void selfn_cores(const char *arg) {
	char *token;
	const char* s = " ";
	char* copy = strdup(arg);
	int i = 0;
	
	token = strtok(copy, s);
	while (token != NULL) {
		core_list[i] = strdup(token);
		token = strtok(NULL, s);
		i++;
	}
}

void print_results_to_csv(FILE *f,char *time, const pid_t pid, int core, struct pqos_event_values *data, struct pqos_event_values *high_prio_data, int j) {
	double ipc = data->ipc;
	double llc = bytes_to_kb(data->llc);
        double mbr = bytes_to_mb(data->mbm_remote_delta) * coeff;
        double mbl = bytes_to_mb(data->mbm_local_delta) * coeff;
	uint64_t instructions;
	uint64_t cycles;
	if (sel_interface == PQOS_INTER_OS)
	{
		if (info_table[high_prio_core]->status == 0)
			cycles = (long unsigned) high_prio_data->ipc_unhalted;
		else
			cycles = info_table[high_prio_core]->cycles + (long unsigned) high_prio_data->ipc_unhalted;

		if (info_table[j]->status == 0)
			instructions = (uint64_t) data->ipc_retired;
		else
			instructions = info_table[j]->instructions + (uint64_t) data->ipc_retired;

		fprintf(f, "%s,%d,%d,%.2f,%llu,%.1f,%.1f,%.1f,%lu,%lu,%d,%d\n", time, pid, core, ipc, (unsigned long long) data->llc_misses_delta, llc, mbl, mbr, instructions, cycles, cores_tmp, allocation[0]);
	}
	else 
	{
		uint64_t cycles = (long unsigned) data->ipc_unhalted;
        	uint64_t instructions = (uint64_t) data->ipc_retired;

		fprintf(f, "%s,%d,%.2f,%llu,%.1f,%.1f,%.1f,%lu,%lu\n", time, core, ipc, (unsigned long long) data->llc_misses_delta, llc, mbl, mbr, instructions, cycles);
	}
}

void allocation_sampling(void){
	int j, core;
	if (CT_favoured == true)
		CT_favoured = false;
	int samples_number = 1;

	/* Apply first sample */
	apply_new_allocation(samples[samples_number-1]);
 
	struct timeval current_time;
	gettimeofday(&current_time, NULL);
	long previous_time_of_sampling = timeval_to_usec(&current_time);
	while(1){
		gettimeofday(&current_time, NULL);
		long cur_time = timeval_to_usec(&current_time);
		if (time_to_take_meas(cur_time, previous_time_of_sampling)){

			/* Take measurements for HP */
			struct pqos_mon_data **sam_data = monitor();
			for(j=0; j<NUM; j++) {
                        	if (sel_interface == PQOS_INTER_OS) {                                                                                                                                                                                                                core = pid_core_num(sam_data[j]->pids[0]);
					if (core < 0)                                                                                                                                                                                                                                        printf("Error retrieving core from PID\n");
                        	}                                                                                                                                                                                                                                            else                                                                                                                                                                                                                                                 core = *sam_data[j]->cores;
				if (core == high_prio_core) {
					sample_ipc[samples_number-1] = sam_data[j]->values.ipc;
					break;
				}
			}
			printf("Sampling with ways for HP: %d\n",samples[samples_number-1]);
			if (samples_number == NUM_SAMPLES){
				/* 
				 * Allocation sampling ended
				 * Find best allocation based on the 98% of best IPC
				 */
				printf("\nAllocation Sampling Ended\n");
				int index = best_allocation(sample_ipc);
				printf("Index is: %d\n",index);
				opt_allocation = samples[index];
				ipc_opt = sample_ipc[index];
				apply_new_allocation(opt_allocation);
				current_allocation = opt_allocation;
				prev_sampling = 1;
				break;
			}
			else{
				/* Move to the next sample */
				samples_number++;
				previous_time_of_sampling = cur_time;
				apply_new_allocation(samples[samples_number-1]);
			}
		}
	}
}

void allocation_optimisation(void) {
	if (stable_bandwidth(bw_hp[0], geometric_mean) == 0 && geometric_mean != 0.0) {
		printf("\nPhase Change -> Allocation Reset\n");
		printf("HP Bandwidth: %f Geometric mean: %f\n", bw_hp[0], BW_UPPER_LIMIT * geometric_mean);
		allocation_reset();
	}
	else if (stable_ipc(ipc[0],ipc[1]) == 1) {
		if (tp_en > 0) {
			max_cores++;
			thread_pack();
			allocation_sampling();
			if (max_cores == 9)
				tp_en = 0;
		}
		else if (current_allocation > 2) {
			printf("\nStable IPC - Current Allocation -= 1\n\n");
			current_allocation--;
			apply_new_allocation(current_allocation);
		}
		else
			printf("\nStable IPC - Lower Allocation reached\n\n");
	}
	else if (better_ipc(ipc[0],ipc[1]) == 0) {
		printf("\nWorse IPC -> Allocation Reset\n");
		allocation_reset();
	}
	else {
		printf("\nPerformance better\n");
		if (tp_en > 0) {                                                                                                                                                                                                                                     max_cores++;
                        thread_pack();                                                                                                                                                                                                                               allocation_sampling();
                        if (max_cores == 9)
                                tp_en = 0;
                }
	}
}

void allocation_reset(void) {
	int j, core;
	double total_bandwidth_res = 0.0, hp_ipc_res= 0.0;

	if (CT_favoured == true) {
		if (current_allocation < 19) {
			prev_reset = 1;
			int rollback_allocation = current_allocation;
			current_allocation = opt_allocation;
			apply_new_allocation(current_allocation);
			struct timeval current_res_time;
			gettimeofday(&current_res_time, NULL);
			long previous_res_time = timeval_to_usec(&current_res_time);
			long cur_res_time = timeval_to_usec(&current_res_time);
			while(!time_to_take_meas(cur_res_time, previous_res_time)) {
				gettimeofday(&current_res_time, NULL);
				cur_res_time = timeval_to_usec(&current_res_time);
			}
			struct pqos_mon_data **res_data = monitor();
			for(j=0;j<NUM;j++) {
				const struct pqos_event_values *res_ptr = &res_data[j]->values;

				if (sel_interface == PQOS_INTER_OS) {                                                                                                                                                                                                                core = pid_core_num(res_data[j]->pids[0]);
					if (core < 0)                                                                                                                                                                                                                                        printf("Error retrieving core from PID\n");
				}                                                                                                                                                                                                                                            else                                                                                                                                                                                                                                                 core = *res_data[j]->cores;
				if (core == high_prio_core)
					hp_ipc_res = res_ptr->ipc;
				total_bandwidth_res = total_bandwidth_res + bytes_to_mb(res_ptr->mbm_local_delta) * coeff_sam;
			}
			if (bandwidth_saturation(transform_MB_to_GB(total_bandwidth_res))) {
				BW_saturated = true;
				printf("\nAllocation Reset: Bandwidth Saturated -> Allocation Sampling\n");
				bw_sat_counter++;
				allocation_sampling();                                                                                                                                                                                                                       prev_sam_sat = 1;
			}
			else if (better_ipc(hp_ipc_res, ipc[0]) == 0) {
				printf("\nAllocation Reset: Performance not better -> Rollback\n");
				current_allocation = rollback_allocation;
				apply_new_allocation(current_allocation);
			}
			else
				printf("\nAllocation Reset: Performance Better\n");
		}
		else
			printf("CT already in charge\n");
	}
	else {
		prev_reset = 1;
		current_allocation = opt_allocation;
		apply_new_allocation(current_allocation);
		struct timeval current_res_time;
                gettimeofday(&current_res_time, NULL);
                long previous_res_time = timeval_to_usec(&current_res_time);
                long cur_res_time = timeval_to_usec(&current_res_time);
                while(!time_to_take_meas(cur_res_time, previous_res_time)) {
                        gettimeofday(&current_res_time, NULL);
                        cur_res_time = timeval_to_usec(&current_res_time);
                }
		printf("New measurement.  Current time : %ld, Previous time : %ld, DIFFERENCE : %ld\n", cur_res_time, previous_res_time, cur_res_time - previous_res_time);
		struct pqos_mon_data **res_data = monitor();
                for(j=0;j<NUM;j++) {
                        const struct pqos_event_values *res_ptr = &res_data[j]->values;

			if (sel_interface == PQOS_INTER_OS) {                                                                                                                                                                                                                core = pid_core_num(res_data[j]->pids[0]);                                                                                                                                                                                                   if (core < 0)                                                                                                                                                                                                                                        printf("Error retrieving core from PID\n");
                        }                                                                                                                                                                                                                                            else                                                                                                                                                                                                                                                 core = *res_data[j]->cores;
                        if (core == high_prio_core)
                                hp_ipc_res = res_ptr->ipc;
                        total_bandwidth_res = total_bandwidth_res + bytes_to_mb(res_ptr->mbm_local_delta) * coeff_sam;
		}
		if (bandwidth_saturation(transform_MB_to_GB(total_bandwidth_res))) {
			BW_saturated = true;
			bw_sat_counter++;
			if (prev_sam_sat || tp_en) {
				printf("\nAllocation Reset: Bandwidth Saturated -> Thread Packing & Allocation Sampling\n");
				max_cores--;
				thread_pack();
				prev_sam_sat = 0;
				tp_en = 1;
				prev_tp = 1;
                        }
                        else {
                                printf("\nAllocation Reset: Bandwidth Saturated -> Allocation Sampling\n");
                                allocation_sampling();                                                                                                                                                                                                                       prev_sam_sat = 1;
                        }
		}
		else if (stable_ipc(hp_ipc_res, ipc_opt) == 0) {
			printf("\nAllocation Reset: Performance is NOT near optimal -> Allocation Sampling\n");
			allocation_sampling();
		}
		else
			printf("\nAllocation Reset: Performance is near optimal\n");
	}
}

int pid_core_num(const pid_t pid) {
	int core;
        char core_s[64];
        char pid_s[64];
        char *tmp;
        int ret;

        if (pid < 0)
                return -1;

        memset(core_s, 0, sizeof(core_s));
        ret = uinttostr_noalloc(pid_s, sizeof(pid_s), pid);
        if (ret < 0)
                return -1;

        ret = get_pid_stat_val(pid_s, PID_COL_CORE, sizeof(core_s), core_s);
        if (ret != 0)
                return -1;

        core = strtoul(core_s, &tmp, 10);
	if (is_str_conversion_ok(tmp) == 0)
                return -1;

        return core;
}

void dicer_init(void){
	int i, ret;
	ret = bind_cores_to_cos(high_prio_core);
	if (ret != PQOS_RETVAL_OK) {
                printf("Failed to poll monitoring data!\n");
                free(mon_grps);
                free(mon_data);
        }
	current_allocation = 19;
	opt_allocation = 19;
	apply_new_allocation(19);
	CT_favoured = true;
	BW_saturated = false;

	for (i=0; i<NUM; i++){
		info_table[i] = malloc(sizeof(struct process_info));
		info_table[i]->status = 0;
		info_table[i]->restarts = 0;
		info_table[i]->instructions = 0;
		info_table[i]->cycles = 0;
		info_table[i]->core = strdup(core_list[i]);
		if (i == high_prio_core)
			info_table[i]->benchmark = strdup(hp_bench);
		else
			info_table[i]->benchmark = strdup(be_bench);
	}

	for (i=0; i<NUM; i++)
		start_a_process(pid_table, script_table, i, info_table[i]->core, info_table[i]->benchmark);
}

void start_a_process(pid_t *ptr_pid, pid_t *sc_pid, int offset, char *core, char *executable_name) {
        pid_t pid;
        int fd[2];

        if (pipe(fd) == -1) {
        	perror("pipe");
        	exit(EXIT_FAILURE);
        }

        if ((pid = fork()) < 0)
                perror("fork() error");
        else if (pid == 0) {
                char fd1[20];
                snprintf(fd1, 20, "%d", fd[1]);
                close(fd[0]);
		
		char *python[8];
		python[0] = strdup("/usr/bin/taskset");
		python[1] = strdup("-c");
		python[2] = strdup(core);
		python[3] = strdup("./run_executables.sh");
		python[4] = strdup(fd1);
		python[5] = strdup(core);
		python[6] = strdup(executable_name);
		python[7] = NULL;
		
                execvp(python[3], python);
        }
        sc_pid[offset] = pid;
        close(fd[1]);

        char buffer[1024];

        while (read(fd[0], buffer, sizeof(buffer)) != 0) {}

	printf("Returned buffer: %s\n",buffer);
        char *ptr;
	pid = strtoul(buffer, &ptr, 10);

        ptr_pid[offset] = pid;
        printf("Started a process %s on core %s with pid %d\n",executable_name, core, pid);
}

int check_instances_and_restart(struct pqos_mon_data **inst_data) {
	pid_t p;
	int status, j, ret;
	double avg_ipc;
	
	for (j=0; j<NUM; j++){
		if ((p = waitpid(script_table[j], &status, WNOHANG)) == -1){
			sleep(1);
			perror("wait() error");
		}
		else if (p == 0){
			//printf("child with pid %d is still running\n", pid_table[i]);
		}
		else{
			if (WIFEXITED(status)){
				printf("child with pid %d exited with status of %d\n", pid_table[j], WEXITSTATUS(status));
				if (info_table[j]->status == 0){
					stopped_processes++;
					info_table[j]->status = 1;
				}
				info_table[j]->restarts++;
				const struct pqos_event_values *ptr = &inst_data[j]->values;
				
				if (j == high_prio_core) {
					high_prio_restarted = 1;
					info_table[j]->cycles = info_table[j]->cycles + (uint64_t) ptr->ipc_unhalted;
				}
				else {
					const struct pqos_event_values *high_prio_values = &inst_data[0]->values;
					if (info_table[0]->status == 0)
                                                info_table[j]->cycles = (uint64_t) high_prio_values->ipc_unhalted;
                                        else {
                                                if (high_prio_restarted == 1)
                                                        info_table[j]->cycles = info_table[0]->cycles;
                                                else
                                                        info_table[j]->cycles = info_table[0]->cycles + (uint64_t) high_prio_values->ipc_unhalted;
                                        }
                                }

				info_table[j]->instructions = info_table[j]->instructions + (uint64_t) ptr->ipc_retired;
				
				if (sel_interface == PQOS_INTER_OS) {
					ret = remove_pid(j);
					if (ret < 0) {
						printf("\nError removing PID\n");
						return 1;
					}
				
					start_a_process(pid_table, script_table, j, info_table[j]->core, info_table[j]->benchmark);

					ret = replace_pid(pid_table[j], j);
					if (ret < 0) {
						printf("\nError replacing PID\n");
						return 1;			
					}
				}
				else
					start_a_process(pid_table, script_table, j, info_table[j]->core, info_table[j]->benchmark);

				if (tp_en && j != high_prio_core)
					thread_pack_pid(pid_table[j]);

				printf("child with pid %d forked\n", pid_table[j]);
			}
			else 
				printf("child with pid %d did not exit successfully", pid_table[j]);

		}
	}

	if (stopped_processes == NUM){
		printf("ALL processes executed at least once\n");
		store_bw_sat_counter();
		for (j=0; j<NUM; j++){
			avg_ipc = (double) (info_table[j]->instructions) / (double) (info_table[j]->cycles);
			store_the_ipc(avg_ipc, j, info_table[j]->restarts);
		}
		for (j=0; j<NUM; j++){
			printf("Signal handler killed child%d with pid %d, %s\n", j, pid_table[j], info_table[j]->benchmark);
			kill(pid_table[j], SIGKILL);
			kill(script_table[j], SIGKILL);
		}
		return 1;
	}
	return 0;
}

void selfn_monitor_file(const char *arg)
{
        selfn_strdup(&sel_output_file, arg);
}

void free_space(void) {
	printf("Free allocated space\n");
	int i;
	for(i=0; i<NUM; i++)
		free(info_table[i]);
}


void build_header_csv(char *hdr, const size_t sz_hdr) {
	ASSERT(hdr != NULL && sz_hdr > 0);
        memset(hdr, 0, sz_hdr);

	if (sel_interface == PQOS_INTER_OS)
	     	strncpy(hdr, "Time,PID,Core,IPC,LLC Misses", sz_hdr - 1);
	else
		strncpy(hdr, "Time,Core,IPC,LLC Misses", sz_hdr - 1);
        strncat(hdr, ",LLC[KB]", sz_hdr - strlen(hdr) - 1);
        strncat(hdr, ",MBL[MB/s],MBR[MB/s]", sz_hdr - strlen(hdr) - 1);
	strncat(hdr, ",Instructions,Cycles", sz_hdr - strlen(hdr) - 1);
}
