#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>

#include <e-hal.h>

#include "papi.h"
#include "parameters.h"
#include "address.h"
#include "types.h"

#define ERR_TOL 0.1

//function to get wall clock time
long_long gettime(){
	return PAPI_get_virt_usec();
}

//functions declarations
void random_shards(KERNEL_T **L1_kernel, KERNEL_T **L2_kernel);
void init_weights(KERNEL_T **L1_kernel, SCALE_T *L1_kernel_scale, KERNEL_T **L2_kernel, SCALE_T *L2_kernel_scale);
void init_test_image(IMAGE_T *image);
MEM_ADDR_T l1map_global_addr(unsigned pe, unsigned offset);
void filter2D(KERNEL_T *kernel, INTERMEDIATE_T *src, INTERMEDIATE_T *dest, unsigned kernel_width, unsigned src_width, SCALE_T scale);
void avepool_subsample(INTERMEDIATE_T *src, MAP_T *dest, unsigned stride, unsigned src_width);
void load_stack(unsigned i, unsigned j, unsigned coreid, e_epiphany_t *dev, unsigned timesteps);

//classification functions
static inline float dot_prod(float *pActivation , float *pWeight, int vLen);
static inline float dot_prod_rotate_weightVec(float *pFeatureVec, float *pWeightVec, int iN, int circShift);
static int index_of_max(float *pVec, int vLen);
void elm_classifier(float *pInputNeurons, int nInputNeurons, int nHiddenNeurons, int nOutputNeurons, float *pInputWt, float *pHiddenBias, float *pOutputWt);

IMAGE_T *image;
KERNEL_T **L1_kernel, **L2_kernel;
MAP_T  **L1_maps, **L2_maps;
MAP_T **L1_maps_parr, **L2_maps_parr;
SCALE_T *L1_kernel_scale, *L2_kernel_scale, **L1_kernel_scale_shard, **L2_kernel_scale_shard;
unsigned *l1map_c, *l2map_c;
KERNEL_T **L1_kernel_shard, **L2_kernel_shard;
MEM_ADDR_T **l1map_addr_shard;

unsigned **L1_shard_map, **L2_shard_map; //to rebuild L1/L2 maps after compute on Epiphany is done

int main(int argc, char **argv){

	unsigned timesteps = atoi(argv[1]);

	printf("Total Deep Learning timesteps = %d\n",timesteps);	

	//declare variables and events to monitor
	unsigned i,j,k;
	long_long t0, t1;

	//malloc vectors
	image = (KERNEL_T *)malloc(IMAGE_SIZE*sizeof(KERNEL_T));
	L1_maps = (MAP_T **)malloc(L1_MAPS*sizeof(MAP_T *));
	L1_maps_parr = (MAP_T **)malloc(L1_MAPS*sizeof(MAP_T *));
	L2_maps = (MAP_T **)malloc(L2_MAPS*sizeof(MAP_T *));
	L2_maps_parr = (MAP_T **)malloc(L2_MAPS*sizeof(MAP_T *));
	L1_kernel = (KERNEL_T **)malloc(L1_MAPS*sizeof(KERNEL_T *));
	L1_kernel_scale = (SCALE_T *)malloc(L1_MAPS*sizeof(SCALE_T));
	L2_kernel = (KERNEL_T **)malloc(L2_MAPS*sizeof(KERNEL_T *));
	L2_kernel_scale = (SCALE_T *)malloc(L2_MAPS*sizeof(SCALE_T));	
	L1_shard_map = (unsigned **)malloc(NUM_PES*sizeof(unsigned *));
	L2_shard_map = (unsigned **)malloc(NUM_PES*sizeof(unsigned *));

	for (i=0;i<NUM_PES;i++){
		L1_shard_map[i] = (unsigned *)malloc(L1_MAX_MAPS_PER_ECORE*sizeof(unsigned));
		L2_shard_map[i] = (unsigned *)malloc(L2_MAX_MAPS_PER_ECORE*sizeof(unsigned));
	}

	for (i=0;i<L1_MAPS;i++){
		L1_maps[i] = (MAP_T *)malloc(L1_MAP_SIZE*sizeof(MAP_T));
		L1_maps_parr[i] = (MAP_T *)malloc(L1_MAP_SIZE*sizeof(MAP_T));
		L1_kernel[i] = (KERNEL_T *)malloc(L1_KERNEL_SIZE*sizeof(KERNEL_T));
	}
	for (i=0;i<L2_MAPS;i++){	
		L2_maps[i] = (MAP_T *)malloc(L2_MAP_SIZE*sizeof(MAP_T));
		L2_maps_parr[i] = (MAP_T *)malloc(L2_MAP_SIZE*sizeof(MAP_T));
		L2_kernel[i] = (KERNEL_T *)malloc(L2_KERNEL_SIZE*sizeof(KERNEL_T));
	}

	/********************************** INITIALIZATION OF ARRAYS **************************************/
	init_test_image(image);
	init_weights(L1_kernel,L1_kernel_scale,L2_kernel,L2_kernel_scale);
	//partition maps into 16 random shards
	random_shards(L1_kernel, L2_kernel);
	//read in weights for classification stage -- i've randomly initialized them for now..
	float *pInputWt = (float *)malloc(L2_MAPS*L2_MAP_SIZE*8*sizeof(float));
	float *pOutputWt = (float *)malloc(NUM_OUTPUT_NEURONS*NUM_HIDDEN_NEURONS*sizeof(float));
	float *pHiddenBias = (float *)malloc(NUM_HIDDEN_NEURONS*sizeof(float));

	for (i=0;i<L2_MAPS*L2_MAP_SIZE*8;i++)
		pInputWt[i] = rand()%5;
	for (i=0;i<NUM_OUTPUT_NEURONS*NUM_HIDDEN_NEURONS;i++)
		pOutputWt[i] = rand()%3;
	for (i=0;i<NUM_HIDDEN_NEURONS;i++)
		pHiddenBias[i] = rand()%2;

	/****************************************** CPU-ONLY solver ******************************************/
	int t;
	//start taking note of time, and start event counters
	t0=gettime();

	for (t=0;t<timesteps;t++){
		//accum vector for L1 maps, since it is fully-connected + it is add-convolve-once		
		INTERMEDIATE_T *accum = (INTERMEDIATE_T *)malloc(11*11*sizeof(INTERMEDIATE_T));//for L2, since it is fully-connected
		for (i=0;i<L1_MAP_SIZE;i++)
			accum[i] = 0;
		
		/****************** LAYER 1 *********************/
		//to store intermediate results
		unsigned conv_size = IMAGE_WIDTH - L1_KERNEL_WIDTH + 1; //22
		INTERMEDIATE_T *filter2D_out = (INTERMEDIATE_T *)malloc(conv_size*conv_size*sizeof(float));
		for (i=0;i<L1_MAPS;i++){
			filter2D(L1_kernel[i],image,filter2D_out,L1_KERNEL_WIDTH,IMAGE_WIDTH,L1_kernel_scale[i]);
			avepool_subsample(filter2D_out,L1_maps[i],DOWN_FAC1,conv_size);
			for (j=0;j<L1_MAP_SIZE;j++)
				accum[j] += L1_maps[i][j];
		}
		/****************** LAYER 2 *********************/	
		conv_size = L1_MAP_WIDTH - L2_KERNEL_WIDTH + 1; //7
		INTERMEDIATE_T *filter2D_out_L2 = (INTERMEDIATE_T *)malloc(conv_size*conv_size*sizeof(INTERMEDIATE_T));
		INTERMEDIATE_T *aligned = (INTERMEDIATE_T *)malloc((conv_size-1)*(conv_size-1)*sizeof(INTERMEDIATE_T));
		for (i=0;i<L2_MAPS;i++){
			for (j=0;j<L1_MAPS;j++){
				filter2D(L2_kernel[i],accum,filter2D_out_L2,L2_KERNEL_WIDTH,L1_MAP_WIDTH,L2_kernel_scale[i]);			
			
				for (j=0;j<conv_size-1;j++){
					for (k=0;k<conv_size-1;k++){
						aligned[j*(conv_size-1)+k] = filter2D_out_L2[j*conv_size+k];
					}
				}
				avepool_subsample(aligned,L2_maps[i],DOWN_FAC2,6);
			}
		}
	}
	
	//stop taking note of time, and stop event counters
	t1=gettime();

	printf("[SEQUENTIAL]Runtime=%lld\n",t1-t0);
	
	/****************************************** CPU-EPIPHANY solver ******************************************/

	//device handlers
	e_platform_t platform;
	e_epiphany_t dev;

	//initialize device
	e_init(NULL);
	e_reset_system();
	e_get_platform_info(&platform);

	// Open a workgroup
	e_open(&dev, 0, 0, platform.rows, platform.cols);

	// CORE initializations
	FLAG_T done = 0xbeefdead;
	for (i = 0; i < platform.rows; i++){
		for (j = 0; j < platform.cols; j++){
			unsigned coreid = i*platform.rows + j;
			load_stack(i,j,coreid,&dev,timesteps);			
			//for reseting done flag
			if (i == 0 && j == 0)	
				e_write(&dev,i,j,DONE_ADDR,&done,sizeof(FLAG_T));
		}
	}

	//load srec
	e_load_group("pe.srec", &dev, 0, 0, platform.rows, platform.cols, E_FALSE);

	//start taking note of time, and start event counters
	t0=gettime();

	e_start_group(&dev);

	done = 0;
	while (done != 0xdeadbeef){
		e_read(&dev,0,0,DONE_ADDR,&done,sizeof(unsigned));
	}
	
	/*************************** Verify functional correctness *****************************/

#ifdef VERIFY
	for (i=0;i<platform.rows;i++){
		for (j=0;j<platform.cols;j++){
			MAP_T read[L1_MAX_MAPS_PER_ECORE*L1_MAP_SIZE];
			MAP_T read1[L2_MAX_MAPS_PER_ECORE*L2_MAP_SIZE];
			e_read(&dev,i,j,L1_MAPS_ADDR,read,L1_MAX_MAPS_PER_ECORE*L1_MAP_SIZE*sizeof(MAP_T));
			e_read(&dev,i,j,L2_MAPS_ADDR,read1,L2_MAX_MAPS_PER_ECORE*L2_MAP_SIZE*sizeof(MAP_T));
			int k;
			for (k=0;k<l1map_c[i*4+j];k++){
				unsigned id = L1_shard_map[i*4+j][k];
				int n;
				for (n=0;n<L1_MAP_SIZE;n++)
					L1_maps_parr[id][n] = read[k*L1_MAP_SIZE+n];
			}
			for (k=0;k<l2map_c[i*4+j];k++){
				unsigned id = L2_shard_map[i*4+j][k];
				int n;
				for (n=0;n<L2_MAP_SIZE;n++)
					L2_maps_parr[id][n] = read1[k*L2_MAP_SIZE+n];
			}
		}
	}

	int cnt_f = 0, cnt_p = 0;
	for (i=0;i<L1_MAPS;i++){
		int mf = 0;
		for (j=0;j<L1_MAP_SIZE;j++){
			if (fabs(L1_maps[i][j]-L1_maps_parr[i][j]) > ERR_TOL){
				cnt_f++;
				mf = 1;
				printf("L1: [map %d,%d] seq = %f, parr = %f\n",i,cnt_f,L1_maps[i][j],L1_maps_parr[i][j]);
			} else {
				cnt_p++;
			}
		}
	}
	
	printf("L1 fail = %d, pass = %d\n",cnt_f,cnt_p);

	cnt_f = 0, cnt_p = 0;
	for (i=0;i<L2_MAPS;i++){
		int mf = 0;
		for (j=0;j<L2_MAP_SIZE;j++){
			if (fabs(L2_maps[i][j] != L2_maps_parr[i][j]) > ERR_TOL){
				cnt_f++;
				mf = 1;
				//printf("L2: [%d] seq = %u, parr = %u\n",cnt_f,L2_maps[i][j],L2_maps_parr[i][j]);
			} else {
				cnt_p++;
			}
		}
	}
	
	printf("L2 fail = %d, pass = %d\n",cnt_f,cnt_p);
#endif

	//flatten L2_maps
	MAP_T *L2_maps_flattened = (MAP_T *)malloc(L2_MAPS*L2_MAP_SIZE*sizeof(MAP_T));
	MAP_T read[L2_MAX_MAPS_PER_ECORE*L2_MAP_SIZE];

	/****************************** CLASSIFICATION *****************************/

	for (t=0;t<timesteps;t++){
		//read L2 maps out
		for (i=0;i<platform.rows;i++){
			for (j=0;j<platform.cols;j++){			
				e_read(&dev,i,j,L2_MAPS_ADDR,read,L2_MAX_MAPS_PER_ECORE*L2_MAP_SIZE*sizeof(MAP_T));
				int k;
				for (k=0;k<l2map_c[i*4+j];k++){
					unsigned id = L2_shard_map[i*4+j][k];
					int n;
					for (n=0;n<L2_MAP_SIZE;n++)
						L2_maps_parr[id][n] = read[k*L2_MAP_SIZE+n];
				}
			}
		}

		//allocate to flattened array
		for (i=0;i<L2_MAPS;i++){
			for (j=0;j<L2_MAP_SIZE;j++){
				L2_maps_flattened[i*L2_MAPS+j] = L2_maps_parr[i][j];
			}
		}
		
		elm_classifier(L2_maps_flattened,L2_MAPS*L2_MAP_SIZE,NUM_HIDDEN_NEURONS,NUM_OUTPUT_NEURONS,pInputWt,pHiddenBias,pOutputWt);
	}

	//stop taking note of time, and stop event counters
	t1=gettime();

	printf("[PARALLEL]Runtime=%lld\n",t1-t0);

	// Close the workgroup
	e_close(&dev);

	// Finalize e-platform connection
	e_finalize();

	return 0;
}

void init_test_image(IMAGE_T *image){
	int i;
	for (i=0;i<IMAGE_SIZE;i++){
		image[i] = (IMAGE_T)(rand()%2);
	}
}

void random_shards(KERNEL_T **L1_kernel, KERNEL_T **L2_kernel){

	int i,j,k;

	//global mallocs
	L1_kernel_shard = (KERNEL_T **)malloc(NUM_PES*sizeof(KERNEL_T *));
	L2_kernel_shard = (KERNEL_T **)malloc(NUM_PES*sizeof(KERNEL_T *));
	L1_kernel_scale_shard = (SCALE_T **)malloc(NUM_PES*sizeof(SCALE_T *));
	L2_kernel_scale_shard = (SCALE_T **)malloc(NUM_PES*sizeof(SCALE_T *));
	l1map_addr_shard = (MEM_ADDR_T **)malloc(NUM_PES*sizeof(MEM_ADDR_T *));
	for (i=0;i<NUM_PES;i++){
		L1_kernel_shard[i] = (KERNEL_T *)malloc(L1_MAX_MAPS_PER_ECORE*L1_KERNEL_SIZE*sizeof(KERNEL_T));
		L2_kernel_shard[i] = (KERNEL_T *)malloc(L2_MAX_MAPS_PER_ECORE*L2_KERNEL_SIZE*sizeof(KERNEL_T));
		L1_kernel_scale_shard[i] = (SCALE_T *)malloc(L1_MAX_MAPS_PER_ECORE*sizeof(SCALE_T));
		L2_kernel_scale_shard[i] = (SCALE_T *)malloc(L2_MAX_MAPS_PER_ECORE*sizeof(SCALE_T));
		l1map_addr_shard[i] = (MEM_ADDR_T *)malloc(L1_MAPS*sizeof(MEM_ADDR_T));
	}
	
	//local variables
	unsigned *placement_L1 = (unsigned *)malloc(L1_MAPS*sizeof(unsigned));
	unsigned *placement_L2 = (unsigned *)malloc(L2_MAPS*sizeof(unsigned));
	unsigned *l1map_addr = (unsigned *)malloc(L1_MAPS*sizeof(unsigned));
	l1map_c = (unsigned *)malloc(NUM_PES*sizeof(unsigned));
	l2map_c = (unsigned *)malloc(NUM_PES*sizeof(unsigned));

	//init counts
	for (i=0;i<NUM_PES;i++){
		l1map_c[i] = 0;
		l2map_c[i] = 0;
	}

	//assign random placement for each L1 map
	for (i=0;i<L1_MAPS;i++){
		unsigned pe = ((unsigned)rand()%NUM_PES);
		//we need to check that this PE is not over the limit
		unsigned orig_pe = pe;
		int success = 0;
		while (!success){
			if (l1map_c[pe] < L1_MAX_MAPS_PER_ECORE){
				placement_L1[i] = pe;
				j = l1map_c[pe]*L1_KERNEL_SIZE;
				for (k=j;k<j+L1_KERNEL_SIZE;k++){
					L1_kernel_shard[pe][k] = L1_kernel[i][k%L1_KERNEL_SIZE];
				}
				L1_shard_map[pe][l1map_c[pe]] = i;
				L1_kernel_scale_shard[pe][l1map_c[pe]] = L1_kernel_scale[i];
				l1map_addr[i] = l1map_global_addr(pe,l1map_c[pe]);
				l1map_c[pe]++;
				success = 1;
			} else {
				pe = (pe+1)%NUM_PES;
				if (pe == orig_pe){
					printf("Cannot find PE to place L1 map in. Exiting.\n");
					exit(1);
				}
			}
		}
	}

	//assign random placement for each L2 map
	for (i=0;i<L2_MAPS;i++){
		unsigned pe = ((unsigned)rand()%NUM_PES);
		//we need to check that this PE is not over the limit
		unsigned orig_pe = pe;
		int success = 0;
		while (!success){
			if (l2map_c[pe] < L2_MAX_MAPS_PER_ECORE){
				placement_L2[i] = pe;
				j = l2map_c[pe]*L2_KERNEL_SIZE;
				for (k=j;k<j+L2_KERNEL_SIZE;k++){
					L2_kernel_shard[pe][k] = L2_kernel[i][k%L2_KERNEL_SIZE];
				}
				L2_shard_map[pe][l2map_c[pe]] = i;
				L2_kernel_scale_shard[pe][l2map_c[pe]] = L2_kernel_scale[i];
				l2map_c[pe]++;
				success = 1;
			} else {
				pe = (pe+1)%NUM_PES;
				if (pe == orig_pe){
					printf("Cannot find PE to place L2 map in. Exiting.\n");
					exit(1);
				}
			}
		}
	}

	//assign L1 -> L2 edge stuff
	for (i=0;i<NUM_PES;i++){
		int index = 0;
		for (j=0;j<L2_MAPS;j++){
			if (placement_L2[j] == i){
				for (k=0;k<L1_MAPS;k++){ //fully-connected
					l1map_addr_shard[i][index] = l1map_addr[k];
					index++;
				}
			}
		}
	}
}

void init_weights(KERNEL_T **L1_kernel, SCALE_T *L1_kernel_scale, KERNEL_T **L2_kernel, SCALE_T *L2_kernel_scale){

	int m,i;	
	for(m=0;m<L1_MAPS;m++) {
		//initialize L1_kernel randomly
		SCALE_T scale = 0.0f;
		for (i=0;i<L1_KERNEL_SIZE;i++){
			L1_kernel[m][i] = (KERNEL_T)(rand()%2); //between 0 and 255			
			scale += L1_kernel[m][i];
		}
		L1_kernel_scale[m] = 1/scale;
	}

	for (m=0;m<L2_MAPS;m++){
		//intialize L2_kernel randomly
		SCALE_T scale = 0.0f;
		for (i=0;i<L2_KERNEL_SIZE;i++){
			L2_kernel[m][i] = (KERNEL_T)(rand()%2); //between 0 and 255
			scale += L2_kernel[m][i];
		}
		L2_kernel_scale[m] = 1/scale;
	}
}

MEM_ADDR_T l1map_global_addr(unsigned pe, unsigned offset){
	
	int col = pe%4;
	int row = (pe-col)/4;
	unsigned z = L1_MAPS_ADDR + L1_MAP_SIZE*offset*sizeof(MAP_T);

	unsigned address = 0x80800000 + row*(0x4000000) + col*(0x100000) + z;

	return address;
}

void filter2D(KERNEL_T *kernel, INTERMEDIATE_T *src, INTERMEDIATE_T *dest, 
	unsigned kernel_width, unsigned src_width, SCALE_T scale){

	unsigned row,col,kernel_row,kernel_col,i;
	int cntr[kernel_width];
	unsigned o_cntr = 0;
	unsigned conv_size = src_width - kernel_width + 1;
	for (row=0;row<conv_size;row++){
		for (i=0;i<kernel_width;i++)
			cntr[i] = (row+i)*src_width;
		for (col=0;col<conv_size;col++){
			float sop = 0.0f;
			for (kernel_row=0;kernel_row<kernel_width;kernel_row++){
				for (kernel_col=0;kernel_col<kernel_width;kernel_col++){
					sop += kernel[kernel_row*kernel_width+kernel_col]*src[cntr[kernel_row]+kernel_col];
				}
			}
			sop = sop*scale;
			*(dest + o_cntr) = sop > 0 ? sop : 0;//non-linearity

			o_cntr++;
			
			for (i=0;i<kernel_width;i++)
				cntr[i]++;
		}
	}
}

void avepool_subsample(INTERMEDIATE_T *src, MAP_T *dest, unsigned stride, unsigned src_width){
	unsigned row,col,i,j;
	int cntr[stride];
	unsigned o_cntr = 0;

	for (row=0;row<src_width;row+=stride){
		for (i=0;i<stride;i++)
			cntr[i] = (row+i)*src_width;
		for (col=0;col<src_width;col+=stride){

			//unrolled version - 2x2 - average pooling
			float sum = src[cntr[0]]+src[cntr[0]+1]+src[cntr[1]]+src[cntr[1]+1];
			sum = sum*0.25f;//divide by 4

			dest[o_cntr] = sum;
			o_cntr++;

			for (i=0;i<stride;i++)
				cntr[i] += stride;
		}
	}
}

void load_stack(unsigned i, unsigned j, unsigned coreid, e_epiphany_t *dev, unsigned timesteps) {
	
	unsigned curr_address = GLOBAL_CONSTANTS_ADDR;

#ifdef DEBUG
	if (coreid == 0)
		printf("Writing timesteps to 0x%x\n",curr_address);
#endif
	e_write(dev,i,j,curr_address,&timesteps,sizeof(GLOBAL_CONSTANTS_T));
	curr_address += 4;

#ifdef DEBUG
	if (coreid == 0)
		printf("Writing l1map_count to 0x%x\n",curr_address);
#endif
	e_write(dev,i,j,curr_address,&l1map_c[coreid],sizeof(GLOBAL_CONSTANTS_T));
	curr_address += 4;

#ifdef DEBUG
	if (coreid==0)
		printf("Writing l2map_count to 0x%x\n",curr_address);
#endif
	e_write(dev,i,j,curr_address,&l2map_c[coreid],sizeof(GLOBAL_CONSTANTS_T));	
	curr_address = IMAGE_ADDR;

#ifdef DEBUG
	if (coreid==0)
		printf("Writing image to 0x%x\n",curr_address);
#endif
	e_write(dev,i,j,curr_address,image,IMAGE_SIZE*sizeof(IMAGE_T));
	curr_address = L1_KERNEL_ADDR;

#ifdef DEBUG
	if (coreid==0)
		printf("Writing L1_kernel to 0x%x\n",curr_address);
#endif
	e_write(dev,i,j,curr_address,L1_kernel_shard[coreid],l1map_c[coreid]*L1_KERNEL_SIZE*sizeof(KERNEL_T));
	curr_address = L1_KERNEL_SCALE_ADDR;

#ifdef DEBUG
	if (coreid==0)
		printf("Writing L1_kernel_scale to 0x%x\n",curr_address);
#endif
	e_write(dev,i,j,curr_address,L1_kernel_scale_shard[coreid],l1map_c[coreid]*sizeof(SCALE_T));
	curr_address = L2_KERNEL_ADDR;

#ifdef DEBUG	
	if (coreid==0)
		printf("Writing L2_kernel to 0x%x\n",curr_address);
#endif
	e_write(dev,i,j,curr_address,L2_kernel_shard[coreid],l2map_c[coreid]*L2_KERNEL_SIZE*sizeof(KERNEL_T));
	curr_address = L2_KERNEL_SCALE_ADDR;

#ifdef DEBUG
	if (coreid==0)
		printf("Writing L2_kernel_scale to 0x%x\n",curr_address);
#endif
	e_write(dev,i,j,curr_address,L2_kernel_scale_shard[coreid],l2map_c[coreid]*sizeof(SCALE_T));
	curr_address = L2_L1_MAP_ADDR;

#ifdef DEBUG
	if (coreid==0)
		printf("Writing l1map_addr to 0x%x\n",curr_address);
#endif
	e_write(dev,i,j,curr_address,l1map_addr_shard[coreid],L1_MAPS*sizeof(MEM_ADDR_T));
}

void elm_classifier(float *pInputNeurons, int nInputNeurons, int nHiddenNeurons, int nOutputNeurons, float *pInputWt, 
	float *pHiddenBias, float *pOutputWt) {
	
	float *pHiddenNeuron, *pOutputNeuron;
	int v, n, label, nWeightVec, nFullyUsedVec, remHiddenNeurons;

	pHiddenNeuron = (float *)malloc(nHiddenNeurons * sizeof(float));
	pOutputNeuron = (float *)malloc(nOutputNeurons * sizeof(float));

	nWeightVec = ceil(nHiddenNeurons / nInputNeurons);
	nFullyUsedVec = floor(nHiddenNeurons / nInputNeurons);
	
	// Input layer computations
	for (v = 0; v < nFullyUsedVec; v++) {
		for (n = 0; n < nInputNeurons; n++) {
			pHiddenNeuron[v * nInputNeurons + n] = dot_prod_rotate_weightVec(pInputNeurons,
				pInputWt + v * nInputNeurons, nInputNeurons, n);
		}
	}

	remHiddenNeurons = nHiddenNeurons - nInputNeurons * nFullyUsedVec;
	
	for ( n = 0; n < remHiddenNeurons; n++) {
		
		pHiddenNeuron[nFullyUsedVec * nInputNeurons + n] = dot_prod_rotate_weightVec(pInputNeurons,
			pInputWt + nFullyUsedVec * nInputNeurons, nInputNeurons, n);
	}
	
	// Add bias to hidden neurons and do nonlinear activation
	for (n = 0;n < nHiddenNeurons;n++){
		pHiddenNeuron[n] += pHiddenBias[n];
		pHiddenNeuron[n] = 1 / ( 1 + exp(-pHiddenNeuron[n]));
	}	

	// Output layer
	for ( n = 0; n < nOutputNeurons; n++) {
		pOutputNeuron[n] = dot_prod(pHiddenNeuron, pOutputWt + n * nHiddenNeurons, nHiddenNeurons);
	}
	
	//Classify
	label = index_of_max(pOutputNeuron, nOutputNeurons);
	//printf("THE PREDICTED LABEL FOR THE GIVEN INPUT IS = %d\n", label);

	//free(pHiddenNeuron);
	//free(pOutputNeuron);
}

static inline float dot_prod(float *pActivation , float *pWeight, int vLen) {
	int e;
	float sop;

	sop = 0;
	for (e = 0; e < vLen; e++) {
		sop += pActivation[e] * pWeight[e];
	}

	return sop;
}

static inline float dot_prod_rotate_weightVec(float *pFeatureVec, float *pWeightVec, int iN, int circShift) {
	float sop;
	int n;

	sop = 0;
	for (n = 0; n < iN - circShift; n++) {
		sop += pFeatureVec[n + circShift] * pWeightVec[n];
	}
	for ( n = 0; n < circShift; n++) {
		sop += pFeatureVec[n] * pWeightVec[iN + n - circShift];
	}

	return sop;	
}

static int index_of_max(float *pVec, int vLen) {
	float max;
	int maxIndex, e;

	max = -10000000;
	maxIndex = -1;
	for ( e = 0; e < vLen; e++) {
		if (pVec[e] > max) {
			max = pVec[e];
			maxIndex = e;
		}
	}
	if ( maxIndex >= 0) {
	} else {
		//printf("No value greater than %f is not found in the array\n", max);
	}

	return maxIndex;
}
