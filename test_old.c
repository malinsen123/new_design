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



int main(){

	kv_item item1;

	int ret = uszram_init();

	struct timespec start1, end1;
	double total_put_time, duration1, total_get_time;

	total_put_time = 0.0;
	total_get_time = 0.0;


	for(int i=0; i< 80000;i++)
	{
		item1.key= i;

		//memcpy(item1.value, "\n",8 );
		generate_value(item1.value,VALUE_SIZE);


		timespec_get(&start1, TIME_UTC);

		ret = uszram_kv_put(item1);

		timespec_get(&end1, TIME_UTC);

		duration1 = end1.tv_sec - start1.tv_sec + (end1.tv_nsec - start1.tv_nsec) / 1000000000.0;

		total_put_time += duration1;

	}

	printf("The total put time is %.3lf\n", total_put_time);

	printf("Wait 5s for get ready\n");
	sleep(1);

	
	/*
	for(int i=0; i< 10000;i++)
	{
		item1.key= i;

		memcpy(item1.value, "fdsff\n",6 );


		ret = uszram_kv_put(item1);


	}

	*/

	
	for(int i=0; i< 10;i++)
	{

		printf("Try to get item with key %d\n",i);
		printf("/////////////////////// \n");


		timespec_get(&start1, TIME_UTC);

		kv_item * item2 = uszram_kv_get(i);

		timespec_get(&end1, TIME_UTC);

		duration1 = end1.tv_sec - start1.tv_sec + (end1.tv_nsec - start1.tv_nsec) / 1000000000.0;

		total_get_time += duration1;

		if(item2 !=NULL)
		{
			free(item2);
			printf("The item value is %s\n", item2->value);
		}
	}

	
	printf("The total get time is %.3lf\n", total_get_time);

	print_status();

	
	/*

	for(int i=0; i< 10;i++)
	{
		item1.key= i;

		memcpy(item1.value, "abcde\n",6 );


		ret = uszram_kv_put(item1);


	}

	


	for(int i=0; i< 10;i++)
	{

		printf("Try to get item with key %d\n",i);
		printf("/////////////////////// \n");
		kv_item * item2 = uszram_kv_get(i);

		if(item2 !=NULL)
			printf("The item value is %s\n", item2->value);
	}


	print_status();

	/*
	for(int i=9; i< 17;i++)
	{
		item1.key= i;

		memcpy(item1.value, "hello\n",6 );



		ret = uszram_kv_put(item1);


	}



	*/


//	printf("THe size of pgtbl is %ld \n", sizeof(pageta))

}

