# Small software renderer + smoothing tests for XBF models (feasibility study, 15.11.2026). Own code.
import sys,numpy as np
from PIL import Image
sys.path.insert(0,'/tmp/xbf'); from xbf import *

def frame_positions(o,fi):
    s=2.0**-o.vanim['unk']
    raw=o.vanim['data'][fi].view(np.int16).reshape(-1,4)
    return raw[:,:3].astype(np.float32)*s

def subdivide(P_frames, tris, uvs, mode):
    # tris: (T,3) vertex ids, uvs: (T,3,2). Returns new frames, tris, uvs. Topology from vertex ids.
    T=len(tris); edges={}
    def eid(a,b):
        k=(min(a,b),max(a,b))
        if k not in edges: edges[k]=len(edges)
        return edges[k]
    tri_e=np.array([[eid(t[0],t[1]),eid(t[1],t[2]),eid(t[2],t[0])] for t in tris])
    nV=P_frames.shape[1]; nE=len(edges)
    # edge -> opposite vertices
    opp={k:[] for k in edges}
    for t in tris:
        for i in range(3):
            a,b,c=t[i],t[(i+1)%3],t[(i+2)%3]; opp[(min(a,b),max(a,b))].append(c)
    nbr=[set() for _ in range(nV)]
    for (a,b) in edges: nbr[a].add(b); nbr[b].add(a)
    boundary={k for k,v in opp.items() if len(v)!=2}
    out=[]
    for P in P_frames:
        E=np.zeros((nE,3),np.float32)
        for (a,b),i in edges.items():
            o=opp[(a,b)]
            if (a,b) in boundary or len(o)!=2: E[i]=(P[a]+P[b])/2
            elif mode=='loop': E[i]=3/8*(P[a]+P[b])+1/8*(P[o[0]]+P[o[1]])
            else: E[i]=(P[a]+P[b])/2+ (1/8)*(P[o[0]]+P[o[1]])/1 - (1/8)*(P[a]+P[b])  # simplified butterfly (no far wings)
        V=P.copy()
        if mode=='loop':
            for v in range(nV):
                n=len(nbr[v])
                if n<3: continue
                beta=(5/8-(3/8+1/4*np.cos(2*np.pi/n))**2)/n
                V[v]=(1-n*beta)*P[v]+beta*sum(P[u] for u in nbr[v])
        out.append(np.concatenate([V,E]))
    nt=[];nu=[]
    for t,(e0,e1,e2),uv in zip(tris,tri_e,uvs):
        a,b,c=t; m0,m1,m2=nV+e0,nV+e1,nV+e2
        ua,ub,uc=uv; u0=(ua+ub)/2; u1=(ub+uc)/2; u2=(uc+ua)/2
        nt+= [[a,m0,m2],[m0,b,m1],[m2,m1,c],[m0,m1,m2]]
        nu+= [[ua,u0,u2],[u0,ub,u1],[u2,u1,uc],[u0,u1,u2]]
    return np.array(out),np.array(nt),np.array(nu)

VFLIP=lambda v: v
def render(P,tris,uvs,tex,size=400,yaw=35,pitch=25,smooth=True):
    th=np.radians(yaw); ph=np.radians(pitch)
    Rz=np.array([[np.cos(th),-np.sin(th),0],[np.sin(th),np.cos(th),0],[0,0,1]])
    Rx=np.array([[1,0,0],[0,np.cos(ph),-np.sin(ph)],[0,np.sin(ph),np.cos(ph)]])
    P=P[:,[0,2,1]]*np.array([1,-1,1])
    Q=P@Rz.T@Rx.T   # x right, y depth, z up
    # vertex normals (area-weighted)
    fn=np.cross(Q[tris[:,1]]-Q[tris[:,0]],Q[tris[:,2]]-Q[tris[:,0]])
    vn=np.zeros_like(Q)
    for i in range(3): np.add.at(vn,tris[:,i],fn)
    vn/=np.linalg.norm(vn,axis=1,keepdims=True)+1e-9
    L=np.array([-0.4,-0.6,0.7]); L/=np.linalg.norm(L)
    sc=size/42.0; cx=size/2; cy=size*0.5
    sx=cx+Q[:,0]*sc; sy=cy-Q[:,2]*sc; dz=Q[:,1]
    img=np.full((size,size,3),40,np.float32); zb=np.full((size,size),1e9)
    tw,thh=tex.shape[1],tex.shape[0]
    for t,uv,f in zip(tris,uvs,fn):
        x=sx[t];y=sy[t]
        x0,x1=int(max(0,np.floor(x.min()))),int(min(size-1,np.ceil(x.max())));y0,y1=int(max(0,np.floor(y.min()))),int(min(size-1,np.ceil(y.max())))
        if x1<x0 or y1<y0: continue
        X,Y=np.meshgrid(np.arange(x0,x1+1)+.5,np.arange(y0,y1+1)+.5)
        den=(y[1]-y[2])*(x[0]-x[2])+(x[2]-x[1])*(y[0]-y[2])
        if abs(den)<1e-9: continue
        w0=((y[1]-y[2])*(X-x[2])+(x[2]-x[1])*(Y-y[2]))/den; w1=((y[2]-y[0])*(X-x[2])+(x[0]-x[2])*(Y-y[2]))/den; w2=1-w0-w1
        m=(w0>=0)&(w1>=0)&(w2>=0)
        if not m.any(): continue
        z=w0*dz[t[0]]+w1*dz[t[1]]+w2*dz[t[2]]
        ys,xs=np.nonzero(m); zz=z[m]; yy=ys+y0; xx=xs+x0
        ok=zz<zb[yy,xx]
        if not ok.any(): continue
        yy,xx,zz=yy[ok],xx[ok],zz[ok]; W0,W1,W2=w0[m][ok],w1[m][ok],w2[m][ok]
        u=W0*uv[0][0]+W1*uv[1][0]+W2*uv[2][0]; v=W0*uv[0][1]+W1*uv[1][1]+W2*uv[2][1]
        col=tex[np.clip((v%1)*thh,0,thh-1).astype(int) if False else np.clip((VFLIP(v)%1)*thh,0,thh-1).astype(int), np.clip((u%1)*tw,0,tw-1).astype(int)]
        if smooth:
            n=W0[:,None]*vn[t[0]]+W1[:,None]*vn[t[1]]+W2[:,None]*vn[t[2]]; n/=np.linalg.norm(n,axis=1,keepdims=True)+1e-9
        else:
            n=np.repeat((f/ (np.linalg.norm(f)+1e-9))[None],len(u),0)
        lam=np.clip(np.abs(n@L),0,1)*0.75+0.35
        img[yy,xx]=col*lam[:,None]; zb[yy,xx]=zz
    return Image.fromarray(np.clip(img,0,255).astype(np.uint8))

def vertex_normals(P,tris):
    fn=np.cross(P[tris[:,1]]-P[tris[:,0]],P[tris[:,2]]-P[tris[:,0]])
    vn=np.zeros_like(P)
    for i in range(3): np.add.at(vn,tris[:,i],fn)
    return vn/(np.linalg.norm(vn,axis=1,keepdims=True)+1e-9)

def pn_subdivide(P_frames,tris,uvs,levels=2):
    # PN triangles (Vlachos et al. 2001): cubic Bezier patch per triangle from corner positions + normals,
    # evaluated on a regular grid. Corners stay where they are, shared edges stay watertight.
    n=levels
    bary=[(i,j,n-i-j) for i in range(n+1) for j in range(n+1-i)]
    idx={b:k for k,b in enumerate(bary)}
    sub=[]
    for i in range(n):
        for j in range(n-i):
            k=n-i-j
            sub.append(((i+1,j,k-1),(i,j+1,k-1),(i,j,k)))
            if k-1>0: sub.append(((i+1,j,k-1),(i+1,j+1,k-2),(i,j+1,k-1)))
    out=[]; newtris=None; newuv=None
    for P in P_frames:
        N=vertex_normals(P,tris)
        verts=[];nt=[];nu=[]
        # edge points shared between neighbouring triangles: key by (vertex pair, position along edge)
        cache={}
        for t,uv in zip(tris,uvs):
            p1,p2,p3=P[t[0]],P[t[1]],P[t[2]]; n1,n2,n3=N[t[0]],N[t[1]],N[t[2]]
            w=lambda a,b,nn: np.dot(b-a,nn)
            b300,b030,b003=p1,p2,p3
            b210=(2*p1+p2-w(p1,p2,n1)*n1)/3; b120=(2*p2+p1-w(p2,p1,n2)*n2)/3
            b021=(2*p2+p3-w(p2,p3,n2)*n2)/3; b012=(2*p3+p2-w(p3,p2,n3)*n3)/3
            b102=(2*p3+p1-w(p3,p1,n3)*n3)/3; b201=(2*p1+p3-w(p1,p3,n1)*n1)/3
            E=(b210+b120+b021+b012+b102+b201)/6; V=(p1+p2+p3)/3; b111=E+(E-V)/2
            ids=[]
            for (a,b_,c) in bary:
                u,v,ww=a/n,b_/n,c/n
                pt=(b300*u**3+b030*v**3+b003*ww**3+b210*3*u*u*v+b120*3*u*v*v+b201*3*u*u*ww+b021*3*v*v*ww+b102*3*u*ww*ww+b012*3*v*ww*ww+b111*6*u*v*ww)
                ids.append(len(verts)); verts.append(pt)
            for s in sub:
                nt.append([ids[idx[s[0]]],ids[idx[s[1]]],ids[idx[s[2]]]])
                nu.append([ (s_[0]/n)*uv[0]+(s_[1]/n)*uv[1]+(s_[2]/n)*uv[2] for s_ in s])
        out.append(np.array(verts,np.float32)); newtris=np.array(nt); newuv=np.array(nu)
    return np.array(out),newtris,newuv
