
total_line=0

for file in $(find . -regex '.*\.c\|.*\.h\|.*\.cpp' )
do
	line_num=$(cat $file | wc -l)
	total_line=$(($total_line+$line_num))
	echo ${file:2} : $line_num	
done

echo total: $total_line

