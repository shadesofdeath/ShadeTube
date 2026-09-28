#pragma once

// A small, hand-written stand-in for YouTube's base.js that reproduces the structural patterns the
// extractors rely on (global lookup array, cipher helper object, split/join callsite, indexed n
// function reference, a URL helper setting alr=yes, and side-effect statements).
// Written for these tests; it contains no YouTube code.

static const char* kSyntheticPlayer = R"js(var _yt_player={};(function(g){var window=this;'use strict';var Q="split;join;reverse;undefined".split(";");
var Xy={Ab:function(a,b){a.splice(0,b)},Cd:function(a){a.reverse()},
Ef:function(a,b){var c=a[0];a[0]=a[b%a.length];a[b%a.length]=c}};
var Zq=function(a){a=a.split("");Xy.Ef(a,3);Xy.Cd(a,45);Xy.Ab(a,2);Xy.Ef(a,61);return a.join("")};
g.cfg=function(){return{signatureTimestamp:20668,other:"x"}};
var Nq=function(a){var b=a.split(""),c=[1,2,3];if(typeof Kx===Q[3])return a;
for(var d=0;d<b.length;d++){b[d]=String.fromCharCode(b[d].charCodeAt(0)+c[d%3])}b.reverse();return b.join("")};
var Apa=[Nq],Kx={};
g.Uv=function(a){var b;a.C&&(b=a.get("n"))&&(b=Apa[0](b),a.set("n",b),Apa.length||Nq(""))};
var Pz=function(a){this.q={};this.base=a};Pz.prototype.set=function(k,v){this.q[k]=v};
Pz.prototype.get=function(k){return this.q[k]};Pz.prototype.clone=function(){return new Pz(this.base)};
Pz.prototype.Tr=function(){var n=this.get("n");n&&this.set("n",Apa[0](n))};
var kG=function(r,z,P){z=z===void 0?"":z;P=P===void 0?"":P;r=new Pz(r);r.set("alr","yes");P&&r.set(z,encodeURIComponent(Zq(decodeURIComponent(P))));return r};
document.createElement("div").appendChild(null);
if(/^https?:/.test(window.location.href))g.ok=1;else g.ok=0;
})(_yt_player);
)js";
