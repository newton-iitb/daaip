#!/bin/bash
#Being modified by Newton

#$1 benchmark1
#$2 benchmark2
#$3 config file
#$4 result folder

pre="/home/newton/research/specTraces/250M/"
pre_res="/home/newton/research/tools/sniper-7.1/results/"
extn=".out"

# lets understand the script with an example
#
#./Script                     B1  B2   CONF  OUTPUT_FOLDER
#./run_benchmarks_dbpv_dyn.sh gcc libq srrip srriptesting
#
# path_to_res=./results_ph2/lbm-gcc
# path_to_trace1=./traces/250M/lbm
# path_to_trace2=./traces/250M/gcc
#/home/newton/research/tools/sniper/sniper_parth/results_abrip/lbm-gcc
path_to_res=$pre_res$3"/"$1
path_to_trace1=$pre$1

tr_list1=()

#
#mkdir $path_to_res
#
# for traces in ./traces/250M/lbm/*.sift
# all traces are being added to the tr_list1
for traces in $path_to_trace1/*.sift
  do
	tr_list1+=($traces)
  done

# for traces in ./traces/250M/gcc/*.sift

for i in {0..3}
  do
    echo "./run-sniper -n 1 -c ./config/stride_only/gainestown$2 -d $path_to_res"/"  --traces=${tr_list1[i]}
	echo "mv $path_to_res"/"sim.out $path_to_res"/"$1$(grep -o "_[0-9]\+"<<<${tr_list1[i]})
    #./run-sniper -n 1 -c gainestown$2 -d $path_to_res"/" -g --perf_model/l3_cache/cache_size=8192 --traces=${tr_list1[i]}
    ./run-sniper -n 1 -c gainestown$2 -d $path_to_res"/" --traces=${tr_list1[i]}

    # we can test if all the 4 files have been created
    filename1=$path_to_res"/"sim.out
    filename2=$path_to_res"/"sim.cfg
    filename3=$path_to_res"/"sim.info
    filename4=$path_to_res"/"sim.stats.sqlite3
    if [[ -f "$filename1" && -f "$filename2" && -f "$filename3" && -f "$filename4" ]]
    then
        echo "Run Fine"
    else
	    #try once more
	    #./run-sniper -n 1 -c gainestown$2 -d $path_to_res"/" -g --perf_model/l3_cache/cache_size=8192 --traces=${tr_list1[i]}
	    ./run-sniper -n 1 -c gainestown$2 -d $path_to_res"/" --traces=${tr_list1[i]}
    fi
    
	mv $path_to_res"/"sim.out $path_to_res"/"$1$(grep -o "_[0-9]\+"<<<${tr_list1[i]})".out"
    mv $path_to_res"/"sim.cfg $path_to_res"/"$1$(grep -o "_[0-9]\+"<<<${tr_list1[i]})".cfg"
    mv $path_to_res"/"sim.info $path_to_res"/"$1$(grep -o "_[0-9]\+"<<<${tr_list1[i]})".info"
    mv $path_to_res"/"sim.stats.sqlite3 $path_to_res"/"$1$(grep -o "_[0-9]\+"<<<${tr_list1[i]})".stats.sqlite3"
  done

