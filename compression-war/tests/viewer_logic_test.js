// Pure JavaScript unit test with inert DOM objects; no browser or network is used.
const fs=require('fs'),path=require('path'),vm=require('vm'),assert=require('assert');
class Element {
  constructor(tag='div'){this.tag=tag;this.children=[];this.value='';this.textContent='';this.style={};this.checked=false;this.width=640;this.height=512;}
  append(...nodes){this.children.push(...nodes)}
  replaceChildren(){this.children=[]}
  get firstChild(){return this.children[0]}
  get lastChild(){return this.children.at(-1)}
  querySelectorAll(tag){return this.children.flatMap(x=>[...(x.tag===tag?[x]:[]),...(x.querySelectorAll?x.querySelectorAll(tag):[])])}
  getContext(){return {fillRect(){},fillText(){},drawImage(){}}}
}
const ids=Object.fromEntries(['play','mode','prev','next','timeline','clock','native','zoom','choices','cards','error','metrics','metric-note','provenance'].map(x=>[x,new Element()]));
ids.mode.value='time';ids.zoom.value='1';
const frame=(index,when)=>({index,fileIndex:index,ordinal:index+1,sourceMs:index*50,presentMs:when,latencyMs:when-index*50,image:`received/${index}.png`,codedBytes:400,packets:2,quality:{ssim_y_8x8:.9}});
const run=(name,decoded)=>({name,label:name,config:{width:192,height:152,fps:20,color:'color',chroma_block:1,gop:10,preset:'slow',slices:4,sharpen:0,upscale:1},summary:{display_acceptance:'PASS'},quality:{},gaps:{max:100},decoded,shown:decoded,source:[0,1,2].map(i=>({index:i,fileIndex:i,sourceMs:i*50}))});
const context={window:{COMPARISON:{runs:[run('baseline',[frame(0,100),frame(2,200)]),run('candidate',[frame(0,90),frame(1,140),frame(2,190)])]}},
 document:{querySelector:s=>ids[s.slice(1)],createElement:t=>new Element(t),createTextNode:s=>({textContent:s})},
 Image:class {constructor(){this.complete=true;this.naturalWidth=192;this.naturalHeight=152}},performance:{now:()=>0},requestAnimationFrame(){},console};
vm.createContext(context);
const html=fs.readFileSync(path.join(__dirname,'../tools/strategy_viewer.html'),'utf8');
const script=html.match(/<script>([\s\S]*?)<\/script>/)[1];vm.runInContext(script,context);
function test(code){return vm.runInContext(code,context)}
test('t=50;render()');assert.match(test('cards[1].foot.textContent'),/无对应/,'received frame shown before arrival');
test('t=150;render()');assert.match(test('cards[1].foot.textContent'),/源帧 0/,'freeze was concealed');assert.match(test('cards[1].foot.textContent'),/年龄 150/,'frozen picture age did not increase');
ids.mode.value='frame';test('t=50;render()');assert.match(test('cards[1].foot.textContent'),/无对应/,'missing same-source frame was replaced');assert.match(test('cards[2].foot.textContent'),/源帧 1/);
test("choose('candidate')");assert.equal(test('cards.length'),2);test("choose('candidate')");assert.equal(test('cards.length'),3);
test('t=50;step(1)');assert.equal(test('t'),100);test('step(-1)');assert.equal(test('t'),50);
console.log('PASS viewer logic: arrival gating, freeze/age, missing same-frame, selection, stepping (not a browser layout test)');
