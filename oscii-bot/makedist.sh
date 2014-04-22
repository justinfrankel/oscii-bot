rm -fr OSCII-bot-src
mkdir OSCII-bot-src/
mkdir OSCII-bot-src/oscii.xcodeproj
cp oscii.xcodeproj/project.pbxproj OSCII-bot-src/oscii.xcodeproj/
cp -a oscii *.cpp *.h *.rc *.ico *.ds? *.sln *.vcxproj *.vcxproj.filters makedist.sh  OSCII-bot-src/

mkdir SampleScripts
cp *.txt SampleScripts/
rm SampleScripts/license.txt SampleScripts/readme.txt

rm OSCII-bot.zip
zip -r9X OSCII-bot.zip OSCII-bot-src/ license.txt readme.txt SampleScripts/*.txt
zip -r9 OSCII-bot.zip OSCII-bot.app 
zip -r9 OSCII-bot.zip OSCII-bot.exe 
