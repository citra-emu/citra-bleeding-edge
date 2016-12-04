:: Example options to pass to ffmpeg to generate high quality gifs
set filters="fps=24,scale=-1:-1:flags=lanczos"
set palette="palette.png"
set paletteops="stats_mode=diff"
set input="CitraLoading.avi"
set output="output.gif"
ffmpeg -v warning -i %input% -vf "%filters%,palettegen=%paletteops%" -y %palette%
ffmpeg -v warning -i %input% -i %palette% -lavfi "%filters% [x]; [x][1:v] paletteuse" -y %output%