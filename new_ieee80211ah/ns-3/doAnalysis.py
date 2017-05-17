import os
import sys

#group = [1, 2, 5, 10]
#slot = [1]
#payload = [49, 54, 59, 64, 69]
#payload = [34, 39, 44, 66, 67]
payload = [60]

for i in payload:
		command = "python Analysis.py trace"+str(i)+"byte"
		print command
		os.system(command);