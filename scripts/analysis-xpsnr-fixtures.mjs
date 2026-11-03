/** Deterministic planar source-code-value fixtures shared only by independent
 * oracle generation and direct ABI tests. No product scoring implementation. */
export const xpsnrCases=[];
for(const depth of [8,10])for(const rate of [{numerator:24,denominator:1},{numerator:30000,denominator:1001},{numerator:60,denominator:1}])for(const distortion of ['noise','identical','near'])
 xpsnrCases.push({width:128,height:96,frames:12,depth,rate,distortion});
xpsnrCases.push({width:1920,height:1080,frames:3,depth:10,rate:{numerator:60,denominator:1},distortion:'noise'}, {width:4096,height:2304,frames:3,depth:8,rate:{numerator:24,denominator:1},distortion:'noise'});
export function xpsnrFrame(spec,index,distorted=false){
 const {width,height,depth,distortion}=spec,unit=depth===8?1:2,shift=depth-8,scale=1<<shift;
 const bytes=Buffer.alloc(width*height*3/2*unit);let offset=0;
 for(let plane=0;plane<3;plane++){
  const w=plane?width/2:width,h=plane?height/2:height;
  for(let y=0;y<h;y++)for(let x=0;x<w;x++){
   let value=(16+(x*7+y*11+index*13+((x>>4)^(y>>3))*31+plane*47)%220)*scale+((x+y+index)%scale);
   if(distorted&&distortion!=='identical'){
    const noise=distortion==='near'?((x+y+index)%997===0?1:0):(((x*17+y*19+index*23)%17)-8)*scale;
    value=Math.max(0,Math.min((1<<depth)-1,value+noise));
   }
   if(unit===1)bytes[offset++]=value;else{bytes.writeUInt16LE(value,offset);offset+=2;}
  }
 }
 return bytes;
}
