-- ============================================================
-- espBridge — 4G 通信桥任务（银尔达物联网平台 Lua 脚本）
--
-- 功能:
--   1. UART 与 ESP32 通信, 处理查询命令 (NET?/IMEI?/TIME?/GPS?/STATUS?)
--   2. 通过 HTTP POST 将车辆数据上报到云函数 /upload
--   3. 通过 MQTT 订阅/发布指令主题 (car001down / car001up)
--   4. 网络数据透传 ESP32
--
-- 串口协议 (ESP32 -> 4G 模块 发送命令, 模块回复 KEY=VALUE):
--   BOOT=OK / NET=OK / GPS=OK ...  开机与状态通知
--   NET?    -> NET=0/1 CSQ=x
--   IMEI?   -> IMEI=xxxx
--   TIME?   -> TIME=yyyy-mm-dd HH:MM:SS
--   GPS?    -> GPS/FIX/SAT/HDOP/LAT/LNG/ALT/HEIGHT/SPEED
--   STATUS? -> 以上全部 + TIME
--
-- 平台: 银尔达 (GinM2M / 合宙) 4G 模块
-- 注意: 网络通道配置见 config.md
-- ============================================================


function

    local taskname="espBridge"

    local nid=1
    local uid=1

    UartStopProRecCh(1)
    PronetStopProRecCh(1)

    GpsInit()
    GpsSetPower(1)

    libgnss.debug(true)

    sys.wait(5)
    ------------------------------------------------
    -- 开机通知ESP32
    ------------------------------------------------
    UartSetSendCh(uid,"BOOT=OK\r\n")

    local lastNetState=-1
    local lastFix=-1

    while true do

        ------------------------------------------------
        -- 网络状态变化通知
        ------------------------------------------------

        local netState=PronetGetNetSta(nid)

        if netState~=lastNetState then

            lastNetState=netState

            if netState==1 then
                UartSetSendCh(uid,"NET=OK\r\n")
            else
                UartSetSendCh(uid,"NET=FAIL\r\n")
            end

        end

        ------------------------------------------------
        -- GPS状态变化通知
        ------------------------------------------------

        local fixState=0

        if libgnss.isFix() then
            fixState=1
        end

        if fixState~=lastFix then

            lastFix=fixState

            if fixState==1 then
                UartSetSendCh(uid,"GPS=OK\r\n")
            else
                UartSetSendCh(uid,"GPS=FAIL\r\n")
            end

        end

        ------------------------------------------------
        -- 串口接收
        ------------------------------------------------

        local rx=UartGetRecChAndDel(uid)

        if rx then

            rx=string.gsub(rx,"[\r\n]","")

            ------------------------------------------------
            -- NET?
            ------------------------------------------------

            if rx=="NET?" then

            local net=0

            if PronetGetNetSta(nid)==1 then
                net=1
            end

            local csq=mobile.csq() or 0

            local msg=
                "NET="..tostring(net).."\r\n"..
                "CSQ="..tostring(csq).."\r\n"

            UartSetSendCh(uid,msg)

            ------------------------------------------------
            -- IMEI?
            ------------------------------------------------

            elseif rx=="IMEI?" then

                UartSetSendCh(
                    uid,
                    "IMEI="..mobile.imei().."\r\n"
                )

            ------------------------------------------------
            -- TIME?
            ------------------------------------------------

            elseif rx=="TIME?" then

                UartSetSendCh(
                    uid,
                    "TIME="..
                    os.date("%Y-%m-%d %H:%M:%S")
                    .."\r\n"
                )

            ------------------------------------------------
            -- GPS?
            ------------------------------------------------
            elseif rx=="GPS?" then

                local gps=0
                local fix=0
                local sat=0
                local hdop=0
                local lat=0
                local lng=0
                local alt=0
                local speed=0
		        local height=0

    ------------------------------------------------
    -- GGA
    ------------------------------------------------

                local gga=libgnss.getGga(2)

                if gga then

                    gps=1

                    sat=gga.satellites_tracked or 0
                    hdop=string.format("%.1f", gga.hdop or 0)
                    lat=string.format("%.7f", gga.latitude or 0)
		            height=string.format("%.1f", gga.height or 0)
                    lng=string.format("%.7f", gga.longitude or 0)
                    alt=string.format("%.1f", gga.altitude or 0)

                    if (gga.fix_quality or 0)>0 then
                        fix=1
                    end

                end

    ------------------------------------------------
    -- RMC
    ------------------------------------------------

                local rmc=libgnss.getRmc(2)

                if rmc then
                    speed=string.format("%.2f", rmc.speed or 0)
                end

    ------------------------------------------------
    -- 返回
    ------------------------------------------------

                local msg=
                    "GPS="..tostring(gps).."\r\n"..
                    "FIX="..tostring(fix).."\r\n"..
                    "SAT="..tostring(sat).."\r\n"..
                    "HDOP="..tostring(hdop).."\r\n"..
                    "LAT="..tostring(lat).."\r\n"..
                    "LNG="..tostring(lng).."\r\n"..
		            "HEIGHT="..tostring(height).."\r\n"..
                    "ALT="..tostring(alt).."\r\n"..
                    "SPEED="..tostring(speed).."\r\n"

                UartSetSendCh(uid,msg)
            ------------------------------------------------
            -- STATUS?
            ------------------------------------------------
            elseif rx=="STATUS?" then

                local net=0
                if PronetGetNetSta(nid)==1 then
                    net=1
                end

                local csq=mobile.csq() or 0

                local gps=0
                local fix=0
                local sat=0
                local hdop=0
                local lat=0
                local lng=0
                local alt=0
		        local height=0
                local speed=0

    ------------------------------------------------
    -- GGA信息
    ------------------------------------------------

                local gga=libgnss.getGga(2)

                if gga then

                    gps=1

                    sat=gga.satellites_tracked or 0
                    hdop=string.format("%.1f", gga.hdop or 0)
                    lat=string.format("%.7f", gga.latitude or 0)
		            height=string.format("%.1f", gga.height or 0)
                    lng=string.format("%.7f", gga.longitude or 0)
                    alt=string.format("%.1f", gga.altitude or 0)

                    if (gga.fix_quality or 0)>0 then
                        fix=1
                    end

                end

    ------------------------------------------------
    -- RMC信息
    ------------------------------------------------

                local rmc=libgnss.getRmc(2)

                if rmc then
                    speed=string.format("%.2f", rmc.speed or 0)
                end

    ------------------------------------------------
    -- 时间
    ------------------------------------------------

                local timestr=os.date("%Y-%m-%d %H:%M:%S")

    ------------------------------------------------
    -- 返回
    ------------------------------------------------

                local msg=
                    "NET="..tostring(net).."\r\n"..
                    "CSQ="..tostring(csq).."\r\n"..
                    "GPS="..tostring(gps).."\r\n"..
                    "FIX="..tostring(fix).."\r\n"..
                    "SAT="..tostring(sat).."\r\n"..
                    "HDOP="..tostring(hdop).."\r\n"..
                    "LAT="..tostring(lat).."\r\n"..
                    "LNG="..tostring(lng).."\r\n"..
                    "ALT="..tostring(alt).."\r\n"..
		            "HEIGHT="..tostring(height).."\r\n"..
                    "SPEED="..tostring(speed).."\r\n"..
                    "TIME="..timestr.."\r\n"

                UartSetSendCh(uid,msg)

            ------------------------------------------------
            -- 其它数据解析为topic
            ------------------------------------------------

            else

                if PronetGetNetSta(nid)==1 then

                    PronetSetSendCh(nid, rx)

                end

            end

        end

        ------------------------------------------------
        -- 网络数据透传ESP32
        ------------------------------------------------

        local netr=PronetGetRecChAndDel(nid)

        if netr then
            UartSetSendCh(uid,netr)
        end

        sys.wait(100)

    end

end
