#!/bin/sh
# Convert the single profiled sample's FEMU console events to absolute timestamps.
set -eu
log=${1:?supply FEMU console log for RQ3-dma-overlap}
out=${2:?supply output directory}
awk '
    function fields() {
        for(k in a) delete a[k]
        for(i=2;i<=NF;i++) {split($i,t,"="); a[t[1]]=t[2]}
    }
    BEGIN {print "session,graph,token,bytes,load_ns,cu_last_consumer_ns,graph_compute_end_ns,dma_start_ns,dma_end_ns,publish_end_ns"}
    /^CACHE_GRAPH / {fields(); compute[a["session"],a["cookie"]]=a["compute"]; next}
    /^CACHE_BLOCK / {
        fields()
        if (a["dma_bytes"]+0 && a["dma"]+0) {
            n++; session[n]=a["session"]; graph[n]=a["cookie"]
            line[n]=a["token"] "," a["dma_bytes"] "," a["load"] "," a["cu"];
            tail[n]=a["dma_start"] "," a["dma"] "," a["publish_end"]
        }
    }
    END {
        for(j=1;j<=n;j++) {
            finish=compute[session[j],graph[j]]
            if (finish+0) {
                print session[j] "," graph[j] "," line[j] "," finish "," tail[j]; good++
            }
        }
        if(!good) exit 1
    }
' "$log" > "$out/RQ3-dma-events.csv"
