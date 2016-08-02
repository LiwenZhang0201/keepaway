# Gnuplot histogram generation script
# Gregory Kuhlmann, 2002

# Color output
set terminal postscript eps color solid "Helvetica" 24

# Black & White output
#set terminal postscript eps monochrome dashed "Helvetica" 24

# Output file
set output "./hist.eps"

# Title
set title ""

# Appearance
#set data style steps
#set nokey
set border 3
set xtics nomirror
set ytics nomirror
set multiplot

# Axes
set xrange [0:]
set xlabel "Episode Duration (seconds)"

set yrange [0:]
set ylabel "Occurences"

# Plot Data
plot \
"1.out",
