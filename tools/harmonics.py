# Render single notes and measure their harmonics, decay (T60) and release.
# usage: python3 tools/harmonics.py build/electricpiano_render '{"param": value}' [notes] [velocities]
import sys, json, subprocess, numpy as np, scipy.io.wavfile as w, os, tempfile
render=sys.argv[1]; params=json.loads(sys.argv[2])
notes=[int(n) for n in (sys.argv[3] if len(sys.argv)>3 else "33,45,57,69,81,93").split(',')]
vels=[int(v) for v in (sys.argv[4] if len(sys.argv)>4 else "40,80,120").split(',')]
dur=float(os.environ.get('DUR','2.5'))
td=tempfile.mkdtemp()
def rend(n,v):
    sc={"patch":"init","params":params,"length":dur+0.5,"events":[{"t":0,"on":n,"vel":v},{"t":dur,"off":n}]}
    p=f"{td}/s.json"; json.dump(sc,open(p,'w'))
    subprocess.run([render,p,"--out",f"{td}/o.wav"],check=True,capture_output=True)
    sr,x=w.read(f"{td}/o.wav"); x=x.astype(float)/2**31
    return sr,x.mean(axis=1)
def harm(seg,sr,f0,nh=8):
    win=np.hanning(len(seg)); X=np.abs(np.fft.rfft(seg*win)); fr=np.fft.rfftfreq(len(seg),1/sr)
    out=[]
    for k in range(1,nh+1):
        f=f0*k
        if f>sr/2-200: out.append(np.nan); continue
        i=np.argmin(abs(fr-f)); out.append(X[max(0,i-3):i+4].max())
    out=np.array(out); return 20*np.log10(out/out[0]+1e-12), 20*np.log10(out[0]*2/win.sum()+1e-12)
print(f"{'note':>4} {'vel':>3} {'peak':>6} {'H1att':>6} | attack H2..H8 (dB re H1)            | sustain(0.6s) H2..H5     | T60(s) relT60")
for n in notes:
    f0=440*2**((n-69)/12)
    for v in vels:
        sr,x=rend(n,v)
        pk=20*np.log10(abs(x).max()+1e-12)
        L=int(0.06*sr); a0=int(0.015*sr)
        ha,l1=harm(x[a0:a0+max(L,int(8*sr/f0))],sr,f0)
        s0=int(0.6*sr); hs,l2=harm(x[s0:s0+int(0.2*sr)],sr,f0)
        # T60 from H1 band envelope 0.3..dur
        t=np.arange(len(x))/sr
        from scipy.signal import butter,sosfiltfilt,hilbert
        sos=butter(2,[f0*0.85,min(f0*1.15,sr/2-10)],btype='band',fs=sr,output='sos')
        env=np.abs(hilbert(sosfiltfilt(sos,x)))
        def db(a,b): return 20*np.log10(env[int(a*sr):int(b*sr)].mean()+1e-12)
        e1,e2=db(0.3,0.4),db(dur-0.4,dur-0.3)
        t60=60*(dur-0.7)/max(1e-3,e1-e2)
        r1=db(dur+0.02,dur+0.06); r2=db(dur+0.2,dur+0.24)
        rt60=60*0.18/max(1e-3,r1-r2)
        print(f"{n:4d} {v:3d} {pk:6.1f} {l1:6.1f} | "+" ".join(f"{h:5.0f}" for h in ha[1:])+" | "+" ".join(f"{h:5.0f}" for h in hs[1:5])+f" | {t60:5.1f} {rt60:5.2f} H1sus {l2:6.1f}")
