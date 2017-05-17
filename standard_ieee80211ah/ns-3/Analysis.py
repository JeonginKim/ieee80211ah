import sys
import os
import re

infile = open(sys.argv[1], "r");
maxAi = -1;
minAi = 100000000000000;
totalAi = 0;
aiCnt = 0;

maxRi = -1;
minRi = 100000000000000;
totalRi = 0;
riCnt = 0;

def UpdateRI(value):
	global maxRi, minRi, totalRi, riCnt;

	if value > maxRi:
		maxRi = value;

	if value < minRi and value != 0:
		minRi = value;
	
	totalRi += value;
	riCnt += 1;


def UpdateAI(value):
	global maxAi, minAi, totalAi, aiCnt;

	if value > maxAi:
		maxAi = value;
	
	if value < minAi and value != 0:
		minAi = value;

	totalAi += value;
	aiCnt += 1;

def DoAnalysis(token):
	if token[3] == "receivedACK":
		UpdateRI(int(token[5]))
	elif token[3] == "NodeID":
		UpdateAI(int(token[8]))

for line in infile:
	token = [];
	cnt = 0;
	for match in re.finditer(r"\w+", line):
		word = match.group();
		if cnt == 0 and word != "At":
			break;
		else:
			token.append(word);
			cnt += 1

	if len(token) != 0:
		DoAnalysis(token);

print "ai max %f min %f avg %f " % (maxAi, minAi, 1.0*totalAi/aiCnt);	
print "ri max %f min %f avg %f thr %f send %f recv %f loss %f " % (maxRi, minRi, 1.0*totalRi/riCnt, 1.0*riCnt*100*8/100.0, aiCnt, riCnt, 100.0 - 1.0*riCnt/aiCnt * 100.0);	
			
