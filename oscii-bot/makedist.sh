rm -fr OSCII-bot-src
mkdir OSCII-bot-src/
mkdir OSCII-bot-src/oscii.xcodeproj
cp oscii.xcodeproj/project.pbxproj OSCII-bot-src/oscii.xcodeproj/
cp -a oscii license.txt *.cpp *.h *.rc *.ico *.ds? makedist.sh  OSCII-bot-src/
zip -r9X OSCII-bot.zip OSCII-bot-src/ *.txt

zip -r9 OSCII-bot.zip OSCII-bot.app 
zip -r9 OSCII-bot.zip OSCII-bot.exe 


