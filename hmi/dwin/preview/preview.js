"use strict";
(async function () {
  const ui = await fetch("../ui.json", {cache:"no-store"}).then(response => {
    if (!response.ok) throw new Error("无法加载屏幕配置");
    return response.json();
  });
  const $ = selector => document.querySelector(selector);
  const screen = $("#screen");
  const ranges = [1,3,10,30,100,300,1000];
  const frequencies = [2000,5000,8000,10000];
  const state = {page:0,field:0,draft:0,range:0,frequency:0,appliedRange:0,appliedFrequency:0,
    scene:"idle",elapsed:0,started:0,batteryAlarm:false};
  const scenes = {idle:0,run:1,fault:3,offline:4};
  function selected() { return state.field===0 ? state.range : state.frequency; }
  function dirty() { return state.page===1 && state.draft!==selected(); }
  function announce(text) { $("#feedback").textContent=text; }
  function elapsedSeconds() {
    return state.elapsed + (state.scene==="run" ? Math.floor((performance.now()-state.started)/1000) : 0);
  }
  function timeText(seconds) {
    if(seconds<0 || seconds>3599999) return "--";
    return [Math.floor(seconds/3600),Math.floor(seconds/60)%60,seconds%60].map(x=>String(x).padStart(2,"0")).join(":");
  }
  function image(file,item) {
    const node=document.createElement("img"); node.src="../"+file+"?v=blue2"; node.alt="";
    Object.assign(node.style,{left:item.x+"px",top:item.y+"px",width:item.width+"px",height:item.height+"px"});
    screen.append(node);
  }
  function render() {
    screen.replaceChildren(); screen.dataset.page=String(state.page);
    screen.dataset.scene=state.scene; screen.dataset.dirty=String(dirty());
    const names=["状态页","挡位设置页"];
    screen.setAttribute("aria-label",names[state.page]+"，演示数据，未连接设备");
    image(ui.pages[state.page].image,{x:0,y:0,width:800,height:480});
    const online=state.scene!=="offline";
    const values={state:scenes[state.scene],battery:online?(state.batteryAlarm?1:0):2,focus:state.field,edit:dirty()?1:0};
    for(const item of ui.icons.filter(x=>x.pages.includes(state.page))) {
      image("assets/icons/"+String(item.first+values[item.name]).padStart(3,"0")+".png",item);
    }
    const choiceRange=state.field===0?state.draft:state.range;
    const choiceFrequency=state.field===1?state.draft:state.frequency;
    const texts={current:state.scene==="run"?"2.240":state.scene==="idle"?"0.000":"--",
      voltage:state.scene==="run"?"22.360":state.scene==="idle"?"0.000":"--",
      range:online?String(ranges[state.appliedRange]):"--",
      frequency:online?String(frequencies[state.appliedFrequency]/1000):"--",
      elapsed:online?timeText(elapsedSeconds()):"--",
      range_choice:String(ranges[choiceRange]),frequency_choice:String(frequencies[choiceFrequency]/1000)};
    for(const field of ui.fields.filter(x=>x.pages.includes(state.page))) {
      const node=document.createElement("span"); node.className="value"; node.dataset.field=field.name;
      node.textContent=texts[field.name];
      Object.assign(node.style,{left:field.x+"px",top:field.y+"px",width:field.width+"px",height:field.height+"px",
        color:field.color,fontSize:field.font_height+"px"}); screen.append(node);
    }
    $("#page-label").textContent=names[state.page];
    document.querySelectorAll("[data-scene]").forEach(button=>button.setAttribute("aria-pressed",String(button.dataset.scene===state.scene)));
  }
  function key(key) {
    if(state.page===0) {
      if(["right","ok","press"].includes(key)) {
        state.page=1; state.draft=selected(); announce("上下选择项目，旋转编码器调整挡位。");
      }
    } else if(key==="left") {
      const abandoned=dirty(); state.draft=selected(); state.page=0;
      announce(abandoned?"已取消未确认的修改，返回状态页。":"已返回状态页。");
    } else if(key==="up" || key==="down") {
      const field=key==="up"?0:1;
      if(field!==state.field) {state.field=field;state.draft=selected();}
      announce("已选中"+(state.field===0?"阻抗挡位":"频率挡位")+"，旋转调整后按下确认。");
    } else if(key==="ok" || key==="press") {
      if(state.field===0) state.range=state.draft; else state.frequency=state.draft;
      if(state.scene==="idle" || state.scene==="run") {
        if(state.field===0) state.appliedRange=state.range; else state.appliedFrequency=state.frequency;
        announce("示例已确认："+(state.field===0?ranges[state.range]+" Ω":frequencies[state.frequency]/1000+" kHz")+"。");
      } else announce("设置请求已确认，当前异常状态未应用；切回运行示例后应用。");
    }
    render();
  }
  function turn(delta) {
    if(state.page===0) {announce("按右键进入设置，再旋转选择挡位。");return;}
    const limit=state.field===0?ranges.length:frequencies.length;
    state.draft=Math.max(0,Math.min(limit-1,state.draft+delta));
    announce(dirty()?"当前为待选挡位，按下编码器或 OK 确认。":"与已确认设置一致。");render();
  }
  for(const name of ["up","left","ok","right","down","press"]) $("#"+name).addEventListener("click",()=>key(name));
  $("#cw").addEventListener("click",()=>turn(1));
  $("#ccw").addEventListener("click",()=>turn(-1));
  document.querySelectorAll("[data-scene]").forEach(button=>button.addEventListener("click",()=>{
    const next=button.dataset.scene;
    if(next===state.scene) return;
    if(state.scene==="run") state.elapsed=elapsedSeconds();
    if(next==="run" && state.scene!=="run") {state.elapsed=0;state.started=performance.now();}
    if(next==="idle" || next==="run") {state.appliedRange=state.range;state.appliedFrequency=state.frequency;}
    state.scene=next;announce("已切换演示状态；页面不连接实际设备。");render();
  }));
  $("#battery-alarm").addEventListener("change",event=>{state.batteryAlarm=event.target.checked;render();});
  function resize() {screen.style.transform=`scale(${$(".screen-stage").clientWidth/800})`;}
  new ResizeObserver(resize).observe($(".screen-stage"));
  render();resize();
  setInterval(()=>{
    const field=screen.querySelector('[data-field="elapsed"]');
    if(field && state.scene==="run") field.textContent=timeText(elapsedSeconds());
  },250);
})().catch(error=>{document.querySelector("#feedback").textContent=error.message;console.error(error);});
