#1048576 = 1024 * 1024
dd if=/dev/zero of=./data/cache_file0 bs=10M count=10240
dd if=/dev/zero of=./data/cache_file1 bs=10M count=10240
dd if=/dev/zero of=./data/cache_file2 bs=10M count=10240
dd if=/dev/zero of=./data/cache_file3 bs=10M count=10240
dd if=/dev/zero of=./data/cache_file4 bs=10M count=10240
dd if=/dev/zero of=./data/cache_file5 bs=10M count=10240
dd if=/dev/zero of=./data/cache_file6 bs=10M count=10240
dd if=/dev/zero of=./data/cache_file7 bs=1G count=0 seek=100
