# JackIir

A frequency equalization digital sound processor written in Java.  This is the JackIir project ported to Java without built-in Jack connectivity. (https://github.com/adiblol/jackiir)  However, audio is processed as +/-1 floating point arrays in the manner of Jack. (https://jackaudio.org/)  

To connect to Jack using Java, use JNAJack. (https://github.com/jaudiolibs/jnajack)

Default settings create a Low-Mid-High 3-band equalizer. 