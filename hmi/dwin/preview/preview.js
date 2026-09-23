"use strict";
(async function () {
  const ui = await fetch("../ui.json").then(response => {
    if (!response.ok) throw new Error("无法加载页面契约");
    return response.json();
  });
  const $ = selector => document.querySelector(selector);
  const screen = $("#screen");
  const state = { scene:"idle", page:1, mode:0, selected:0, auto:1, enabled:false,
    frequency:2000, cc:0, va:0, range:2, angle:0, frame:0, paused:false };
  const ranges = [1,3,10,30,100,300,1000];
  const names = {boot:"启动检查 · 示例阶段",idle:"待机 · 等待使能",run:"运行 · 示例读数",switching:"切档 · 读数无效",fault:"保护 · 不会自动重启",stale:"数据过期 · 读数隐藏"};
  const readings = {z:9.982,r:9.613,x:2.689,phase:15.6,voltage:22.360,current:2.240,
    apparent:50.086,power:48.235,frequency:2000,ntc1:34.2,ntc2:31.8,ntc3:42.1,bus:36.480};
  let imageNodes = new Map();
  function img(file, x, y, width, height, key) {
    const node = document.createElement("img");
    node.src = "../" + file;
    node.alt = "";
    Object.assign(node.style,{left:x+"px",top:y+"px",width:width+"px",height:height+"px"});
    screen.append(node);
    if(key) imageNodes.set(key,node);
    return node;
  }
  function iconFile(index) { return "assets/icons/" + String(index).padStart(3,"0") + ".png"; }
  function announce(message) { $("#feedback").textContent = message; }
  function syncButtons() {
    $("#enable").checked = state.enabled;
    $("#cc").setAttribute("aria-pressed",String(state.mode === 0));
    $("#va").setAttribute("aria-pressed",String(state.mode === 1));
    document.querySelectorAll("[data-scene]").forEach(button => button.setAttribute("aria-pressed",String(button.dataset.scene===state.scene)));
  }
  function render() {
    screen.replaceChildren(); imageNodes = new Map();
    state.page = state.scene === "boot" ? 0 : state.scene === "fault" ? 2 : 1;
    screen.dataset.page = String(state.page); screen.dataset.scene = state.scene;
    img(ui.pages[state.page].image,0,0,800,480).classList.add("background");
    const iconValues = { selected:state.selected, mode:state.mode, target_unit:state.mode,
      auto_range:state.auto, state:({idle:0,run:1,switching:2,fault:3,stale:4,boot:0})[state.scene],
      fault:1,battery:state.scene==="run"?0:2,charger:state.scene==="run"?0:2,
      calibration:state.scene==="run"?1:2,boot_step:2 };
    for(const item of ui.icons.filter(item=>item.pages.includes(state.page))) {
      img(iconFile(item.first+iconValues[item.name]),item.x,item.y,item.width,item.height,item.name);
    }
    for(const item of ui.animations.filter(item=>item.pages.includes(state.page))) {
      const active = item.name==="boot" || state.scene==="run";
      img(iconFile(active?item.first+state.frame:item.stop),item.x,item.y,item.width,item.height,item.name);
    }
    const valid = state.scene === "run";
    for(const field of ui.fields.filter(field=>field.pages.includes(state.page))) {
      let value = "--";
      if(field.name==="target") value = (state.mode?state.va:state.cc).toFixed(3);
      else if(field.name==="range") value = ranges[state.range].toFixed(3);
      else if(field.name==="request_frequency") value = state.frequency.toFixed(0);
      else if(valid) value = (field.name==="frequency"?Math.round(170000000/(85*Math.round(170000000/(85*state.frequency)))):readings[field.name]).toFixed(field.decimals);
      if(value.length>field.max_chars) value="--";
      const node = document.createElement("span");
      node.className="value"; node.dataset.field=field.name; node.textContent=value;
      Object.assign(node.style,{left:field.x+"px",top:field.y+"px",width:field.width+"px",height:field.height+"px",fontSize:field.font_height+"px"});
      screen.append(node);
    }
    $("#scene-label").textContent=names[state.scene];
    screen.setAttribute("aria-label",names[state.scene]+"；"+(valid?"演示读数，非设备实测":"无实时测量"));
    syncButtons();
  }
  function stop(message) { state.scene="idle"; announce(message); }
  function setScene(scene) {
    state.scene=scene; state.enabled=scene==="run"||scene==="switching";
    if(scene==="run") { state.cc=2.240; state.va=50; state.range=2; }
    announce(scene==="fault"?"保护立即覆盖动画。恢复后必须重新释放并合上使能。":"场景按钮仅切换演示状态，不连接设备。");
    render();
  }
  function turn(detents) {
    state.angle+=detents*18; $("#knob").style.transform=`rotate(${state.angle}deg)`;
    if(state.scene==="fault"||state.scene==="boot") { announce("当前只显示状态，先切换到待机示例。"); return; }
    if(state.selected===0) state.frequency=Math.max(2000,Math.min(10000,state.frequency+detents*100));
    if(state.selected===1) {
      const key=state.mode?"va":"cc", step=state.mode ? 0.1 : 0.01, max=state.mode?50:7.07;
      state[key]=Math.round(Math.max(0,Math.min(max,state[key]+detents*step))*1000)/1000;
      if(state.enabled && state[key]===0) {
        stop("目标归零已停止；调大后仍需释放再合上使能。");
        render();
        return;
      }
    }
    if(state.selected===2 && !state.auto) state.range=Math.max(0,Math.min(6,state.range+detents));
    announce(state.selected===2&&state.auto?"自动量程由控制器选择，长按可切手动。":"已调整当前选中参数；示例测量读数不随旋钮伪造变化。");
    render();
  }
  $("#increase").addEventListener("click",()=>turn(1));
  $("#decrease").addEventListener("click",()=>turn(-1));
  $("#press").addEventListener("click",()=>{state.selected=(state.selected+1)%3;render();announce("已选中"+["频率","目标值","量程"][state.selected]);});
  $("#long-press").addEventListener("click",()=>{state.auto=1-state.auto;render();announce(state.auto?"自动量程":"手动量程");});
  $("#enable").addEventListener("change",event=>{
    state.enabled=event.target.checked;
    if(!state.enabled && (state.scene==="fault"||state.scene==="stale"||state.scene==="boot")) announce("使能已释放；当前异常或检查状态仍保留。");
    else if(!state.enabled) stop("使能已释放；有效目标下可重新合上。");
    else if(state.scene==="fault"||state.scene==="boot"||state.scene==="stale") announce("当前状态不允许启动。");
    else if((state.mode?state.va:state.cc)<=0) announce("目标为零，未启动。调整后需释放再合上使能。");
    else { state.scene="run"; announce("运行演示已启用；显示固定样例，不代表真实输出。"); }
    render();
  });
  for(const [id,mode] of [["cc",0],["va",1]]) $("#"+id).addEventListener("click",()=>{
    if(state.mode!==mode && state.enabled && (state.scene==="run"||state.scene==="switching")) stop("模式切换已请求停止，重新释放再合上使能后才启动。");
    state.mode=mode; render();
  });
  document.querySelectorAll("[data-frequency]").forEach(button=>button.addEventListener("click",()=>{state.frequency=Number(button.dataset.frequency);render();announce("已选择预设频率。");}));
  document.querySelectorAll("[data-scene]").forEach(button=>button.addEventListener("click",()=>setScene(button.dataset.scene)));
  $("#pause").addEventListener("change",event=>{state.paused=event.target.checked;});
  const reduceMotion=matchMedia("(prefers-reduced-motion: reduce)");
  function resize() { screen.style.transform=`scale(${$('.screen-stage').clientWidth/800})`; }
  new ResizeObserver(resize).observe($(".screen-stage"));
  render();resize();
  setInterval(()=>{
    if(state.paused||reduceMotion.matches||document.hidden) return;
    state.frame=(state.frame+1)%12;
    for(const animation of ui.animations) {
      const node=imageNodes.get(animation.name);
      if(node&&(animation.name==="boot"||state.scene==="run")) node.src="../"+iconFile(animation.first+state.frame);
    }
  },80);
})().catch(error=>{ document.querySelector("#feedback").textContent=error.message; console.error(error); });
