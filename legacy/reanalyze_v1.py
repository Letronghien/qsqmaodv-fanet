"""Re-analysis of QS-QMAODV results: paired tests, bootstrap CIs, Holm correction."""
import pandas as pd, numpy as np
from scipy import stats
import sys
R=(sys.argv[1] if len(sys.argv)>1 else 'legacy/data_v1_ns3.40').rstrip('/')+'/'
rng=np.random.default_rng(2026)

def load():
    fam={}
    N=pd.read_csv(R+'family_N.csv'); N['x']=N.nNodes; fam['N']=N
    L=pd.read_csv(R+'family_L.csv'); L['x']=L.PktInterval_s; fam['L']=L
    C=pd.read_csv(R+'family_C.csv'); C['x']=C.PktInterval_s.map({1.0:'L1',0.5:'L2',0.25:'L3',0.1:'L4',0.05:'L5'}); fam['C']=C
    S=pd.read_csv(R+'family_S.csv'); S['x']=np.repeat([5,15,25,35,45],120); fam['S']=S
    E=pd.read_csv(R+'family_E2.csv'); E=E[E.InitEnergy_J==1.0].copy(); E['x']='any E0'; fam['E2']=E
    return fam

def cliff(a,b):
    a=np.asarray(a)[:,None]; b=np.asarray(b)[None,:]
    return float(((a>b).sum()-(a<b).sum())/(a.size*b.size))

def boot_ci(d,B=10000):
    idx=rng.integers(0,len(d),(B,len(d)))
    m=d[idx].mean(1); return np.percentile(m,[2.5,97.5])

def holm(p):
    p=np.asarray(p); o=np.argsort(p); m=len(p); adj=np.empty(m); run=0
    for k,i in enumerate(o):
        run=max(run,(m-k)*p[i]); adj[i]=min(1,run)
    return adj

METRICS={'PDR':100,'Delay_ms':1,'NRL':1}   # PDR in percent points
rows=[]
fam=load()
for F,df in fam.items():
    for x,g in df.groupby('x',sort=False):
        qs=g[g.Protocol=='QS2MAODV'].set_index('Seed')
        for base in ['QMAODV','AODV','PMAODV']:
            b=g[g.Protocol==base].set_index('Seed')
            seeds=qs.index.intersection(b.index)
            for m,sc in METRICS.items():
                a=qs.loc[seeds,m].values*sc; c=b.loc[seeds,m].values*sc; d=a-c
                w=stats.wilcoxon(a,c,zero_method='zsplit') if np.any(d!=0) else None
                mw=stats.mannwhitneyu(a,c,alternative='two-sided')
                lo,hi=boot_ci(d)
                rows.append(dict(Family=F,Cond=x,Baseline=base,Metric=m,n=len(d),
                    mean_QS=a.mean(),mean_B=c.mean(),Delta=d.mean(),CI_lo=lo,CI_hi=hi,
                    p_wilcoxon=w.pvalue if w else 1.0,p_mw_2s=mw.pvalue,
                    p_mw_1s=stats.mannwhitneyu(a,c,alternative='greater' if m=='PDR' else 'less').pvalue,
                    cliff=cliff(a,c)))
T=pd.DataFrame(rows)
# Holm correction within each (Baseline, Metric) across all family×condition cells
T['p_holm']=T.groupby(['Baseline','Metric']).p_wilcoxon.transform(holm)
T['sig_holm']=np.where(T.p_holm<0.05,'yes','no')
import os; os.makedirs('reports/legacy_v1',exist_ok=True)
T.to_csv('reports/legacy_v1/reanalysis_all_conditions.csv',index=False,float_format='%.4f')
pd.set_option('display.width',250); pd.set_option('display.max_rows',400)
q=T[T.Baseline=='QMAODV']
for m in METRICS:
    print('\n=====',m,'QS vs QMAODV (paired Wilcoxon, Holm across 26 cells)')
    print(q[q.Metric==m][['Family','Cond','mean_QS','mean_B','Delta','CI_lo','CI_hi','p_mw_1s','p_wilcoxon','p_holm','cliff']].round(4).to_string(index=False))
# Family-level aggregate (paper Table 7 check)
agg=q.groupby(['Family','Metric']).Delta.mean().unstack()
print('\nAggregate mean Δ vs QMAODV (paper Table 7 check):'); print(agg.round(2))
for base in ['AODV','PMAODV']:
    print(f'\nAggregate mean Δ vs {base}:'); print(T[T.Baseline==base].groupby(['Family','Metric']).Delta.mean().unstack().round(2))
