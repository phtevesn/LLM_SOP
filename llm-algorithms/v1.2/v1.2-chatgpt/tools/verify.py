#!/usr/bin/env python3
"""Check matching CSV rows against original matrix; does not certify optimality."""
import csv
from pathlib import Path
import re
import sys

def main():
    if len(sys.argv)!=3:
        raise SystemExit('Usage: python3 tools/verify.py INPUT.sop RESULTS.csv')
    file,results=map(Path,sys.argv[1:]);text=file.read_text()
    n=int(re.search(r'^\s*DIMENSION\s*:?\s*(\d+)',text,re.M|re.I)[1])
    tail=re.split(r'EDGE_WEIGHT_SECTION',text,flags=re.I)[1].split()
    assert int(tail[0])==n
    values=list(map(int,tail[1:1+n*n]));assert len(values)==n*n
    count=0
    with results.open(newline='') as handle:
        for row in csv.DictReader(handle):
            if row['Instance']!=file.name:continue
            count+=1;assert int(row['Instance Size'])==n
            path=list(map(int,row['Best Path'].split()));cost=int(row['Best Cost'])
            if cost==-1:
                assert not path and row['Optimal']=='no'
                print(f'Row {count}: no feasible solution reported; no optimality claim.')
                continue
            assert sorted(path)==list(range(n)), 'Not a permutation'
            pos={x:k for k,x in enumerate(path)}
            assert all(pos[j]<pos[i] for i in range(n) for j in range(n) if i!=j and values[i*n+j]<0), 'Precedence violation'
            edges=[values[a*n+b] for a,b in zip(path,path[1:])]
            assert all(x>=0 for x in edges)
            assert sum(edges)==cost, 'Cost mismatch'
            assert row['Optimal'] in ['yes','no']
            print(f'Row {count}: valid path, cost {cost}; solver reports Optimal={row["Optimal"]}.')
    assert count, 'No matching rows (CSV must include a header)'
    print('Feasibility/cost validated. Optimality is not independently certified by this checker.')

if __name__=='__main__':main()
