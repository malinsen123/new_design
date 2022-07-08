#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <assert.h>
#include <pthread.h>
#include <unistd.h>
#include <math.h>
#include <time.h>


#include "def_helper.h"
#include "operation.h"


#define LOAD_NUM 5000000
#define TEST_NUM 10

static void *rand_string(char *str, size_t size)
{
    const char charset[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJK";
    if (size) {
        --size;
        for (size_t n = 0; n < size; n++) {
            int key = rand() % (int) (sizeof charset - 1);
            str[n] = charset[key];
        }
        str[size] = '\0';
    }

    return NULL;
}

static void *generate_value(char * value, size_t size)
{
	for(int i =0; i<size;i++)
	{
		if(i%3 == 0)
		{
			value[i] ='a';
		}else if(i%3 == 1)
		{
			value[i]='b';
		}else
		{
			value[i]='c';
		}

	}
	return NULL;

}



static uint_least64_t rand_int(uint_least64_t lower, uint_least64_t upper)
{
	uint_least64_t random_value = (rand()%(upper-lower))+ lower;
	return random_value;
}


static uint_least64_t hash1(uint_least64_t key)
{
	
	char * keystr = malloc(20);

	//char * ptr = keystr;

	sprintf(keystr, "%lu", key);


	uint_least64_t hash = 5381;
	int c;

	while((c = tolower(*(keystr)++)))
	{	
		hash = ((hash << 5) + hash) +c;

	}


	return hash;
}




typedef struct _test_argu{
	int id;
	int test_num;
	int key_num;
	int iter;
	int nb_worker;
	int nb_client;
	char ** key_names;
	double time_spend;

}test_argu;




void * load_the_data(uint_least64_t kv_nums)
{

	kv_item load_kv;

	for(uint_least64_t i = 0;i < kv_nums; i++)
	{
		uint_least64_t load_key = hash1(i);
		load_kv.key = load_key;
 		generate_value(load_kv.value, VALUE_SIZE);

 		#if DEBUG_MODE
		printf("Try to load the kv item with key %lu\n", load_kv.key);
		printf("The kv value is %s\n", load_kv.value);
		#endif

		int ret=uszram_kv_put(load_kv);
	}
}


void *YCSB_C(void * thread_argu)
{

	test_argu * argu=(test_argu*)thread_argu;

	
	double * times = malloc(sizeof(double)*argu->test_num);

	struct timespec start, end;
	double duration, total_duration;

	total_duration = 0.0;

	for(int i=0;i<argu->test_num;i++)
	{
		//printf("The key data is %s\n",key_records[i]);
		srand(i+argu->id*argu->test_num);
		uint_least64_t index = rand_int(0,argu->key_num);
		uint_least64_t test_key = hash1(index);

		//printf("The i value is %d and index is %d\n",i,index);

		#if DEBUG_MODE
			printf("Try to find the key %lu\n",test_key);
		#endif


		timespec_get(&start,TIME_UTC);
		kv_item* ret = uszram_kv_get(test_key);
		timespec_get(&end,TIME_UTC);

		duration = end.tv_sec - start.tv_sec + (end.tv_nsec - start.tv_nsec) / 1000000000.0;
		times[i] = duration;
		total_duration +=duration;


		if(ret!=NULL)
		{
			
			#if DEBUG_MODE
			printf("The value data is %s\n",ret->value);
			#endif
		}
	}


	/*

	char output_file[60];
	sprintf(output_file,"id%d_worker%d_client%d_iter_YCSB_C%d",argu->id,argu->nb_worker,argu->nb_client,argu->iter);
	FILE * outfile = fopen(output_file,"w+");

	for(int i=0;i<argu->test_num;i++)
	{
		fprintf(outfile, "%.10f\n",times[i]);
	}

	
	fclose(outfile);
	*/

	free(times);

	pthread_exit(NULL);


}



void *YCSB_full_update(void * thread_argu)
{

	test_argu * argu=(test_argu*)thread_argu;

	
	double * times = malloc(sizeof(double)*argu->test_num);

	struct timespec start, end;
	double duration, total_duration;

	total_duration = 0.0;

	kv_item test_kv;


	for(int i=0;i<argu->test_num;i++)
	{

		//printf("The key data is %s\n",key_records[i]);
		srand(i+argu->id*argu->test_num);
		uint_least64_t index = rand_int(0,argu->key_num);
		uint_least64_t test_key = hash1(index);

		test_kv.key = test_key;

 		generate_value(test_kv.value, VALUE_SIZE);

 		for(int i=0;i<VALUE_SIZE/10;i++)
 		{
 			int j = rand_int(0, VALUE_SIZE);
 			test_kv.value[j] = 'x';
 		}
		//printf("The i value is %d and index is %d\n",i,index);

		timespec_get(&start,TIME_UTC);
		int ret = uszram_kv_put(test_kv);
		timespec_get(&end,TIME_UTC);

		duration = end.tv_sec - start.tv_sec + (end.tv_nsec - start.tv_nsec) / 1000000000.0;
		times[i] = duration;
		total_duration +=duration;


	}


	/*
	char output_file[60];
	sprintf(output_file,"id%d_worker%d_client%d_iter%d_YCSB_UPDATE",argu->id,argu->nb_worker,argu->nb_client,argu->iter);
	FILE * outfile = fopen(output_file,"w+");

	for(int i=0;i<argu->test_num;i++)
	{
		fprintf(outfile, "%.10f\n",times[i]);
	}

		
	fclose(outfile);
	*/
	free(times);

	printf("Total_time spent for worker %d is %f\n", argu->id, total_duration);

	pthread_exit(NULL);


}




int main(int argc, char ** argv){

	int nb_worker = atoi(argv[1]);
	int test_mode = atoi(argv[2]);
	int nb_client = atoi(argv[3]);
	int test_num =  atoi(argv[4]);


	int ret = uszram_init();

	//load the data

	load_the_data(LOAD_NUM);
	pthread_t tids[nb_client];
	test_argu thread_data[nb_client];



	if(test_mode ==0) // YCSB-C
	{

		for(int i=0;i<nb_client;i++)
		{
			thread_data[i].id=i;
			thread_data[i].test_num = test_num/nb_client;
			//thread_data[i].iter = 0;
			thread_data[i].nb_worker = nb_worker;
			//thread_data[i].nb_client = nb_client;
			//thread_data[i].key_num = load_num;
			//pthread_create(&tids[i],NULL,YCSB_C,(void *)&thread_data[i]);
		
		}

		for(int i =0;i<nb_client;i++)
		{
			pthread_join(tids[i],NULL);

		}

	}else if(test_mode ==1)
	{

		for(int i=0;i<nb_client;i++)
		{
			thread_data[i].id=i;
			thread_data[i].test_num = test_num/nb_client;
			//thread_data[i].iter = 0;
			thread_data[i].nb_worker = nb_worker;
			//thread_data[i].nb_client = nb_client;
			//thread_data[i].key_num = load_num;
			//pthread_create(&tids[i],NULL,YCSB_full_update,(void *)&thread_data[i]);
		
		}

		for(int i =0;i<nb_client;i++)
		{
			pthread_join(tids[i],NULL);

		}


	}

	sleep(5);

	print_status();


}

