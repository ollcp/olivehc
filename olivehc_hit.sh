#!/bin/bash
#by:lidongye@baidu.com
#hit ratio  olivehc
echo "========================================" 
echo "olivehc_hit_ratio"
echo "usage:no argu"
echo '
listen 9139 #image
listen 9140 #cfg
listen 9141 #beijing_image
listen 9142 #beijing_cfg
listen 9150 #2d
listen 9151 #3d'
echo "========================================"
echo status | nc 127.1 5210 |  \
grep -A 100 'listen' | \
awk '{if ($12 > 0) {print $12 "\t" $14 "\t" $2} }' | \
awk ' NR==1 {printf "%10s\t%10s\t%10s\t%10s\n",$1,$2,"ratio",$3} \
      NR>=2 { ratio = $2*100/$1 ; printf "%10s\t%10s\t%10s%\t%10s\n",$1,$2,ratio,$3 } '
echo "========================================"
echo status | nc 127.1 5210 |  \
grep -A 100 'listen' | \
awk '{if ($19 > 0) {print $19 "\t" $21 "\t" $2} }' | \
awk ' NR==1 {printf "%10s\t%10s\t%10s\t%10s\n",$1,$2,"ratio",$3} \
      NR>=2 { ratio = $2*100/$1 ; printf "%10s\t%10s\t%10s%\t%10s\n",$1,$2,ratio,$3 } '

echo "========================================"
echo "last 60seconds hit ratio"
echo status | nc 127.1 5210 |  \
grep -A 100 'listen' | \
awk '{if ($13 > 0) {print $13 "\t" $15 "\t" $2} }' | \
awk ' NR==1 {printf "%10s\t%10s\t%10s\t%10s\n",$1,$2,"ratio",$3} \
      NR>=2 { ratio = $2*100/$1 ; printf "%10s\t%10s\t%10s%\t%10s\n",$1,$2,ratio,$3 } '

echo "========================================"
echo "device used percent:"
echo status | nc 127.1 5210 | grep '++' | awk '{ratio = $4*100/$3 ; printf "%10s:%s%\n",$2,ratio}'
