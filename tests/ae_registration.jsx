(function(){
 var f=new File("D:/Developer/FlareLensAE/build/ae_registration.log");
 f.encoding="UTF-8";f.open("w");
 try{
  f.writeln("AE="+app.version+" effects="+app.effects.length);
  var found=false;
  for(var i=0;i<app.effects.length;i++){
   var e=app.effects[i];
   if(e.matchName=="FlareLensAE_v1"||e.displayName=="FlareSimAE"){
    f.writeln("FOUND "+e.displayName+" "+e.matchName+" "+e.version);
    found=true;
   }
  }
  f.writeln("registered="+found);
 }catch(e){f.writeln("FAIL "+e.toString());}
 f.close();
})();
