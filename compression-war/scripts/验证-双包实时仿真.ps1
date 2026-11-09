param([switch]$Full)
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
Push-Location $root
try {
    if(-not (Test-Path -LiteralPath (Join-Path $root '素材\car_raw.bgr'))){throw '完整回归需要真实素材\car_raw.bgr；新电脑可先按README生成演示视频，或重放evidence中的固定收包证据。'}
    $exe=Join-Path $root 'cw_live.exe';$ff=Join-Path $root 'ffmpeg.exe';$verifier=Join-Path $root 'cw_wire_verify.exe'
    $dir=Join-Path 'runs\two-packet-validation' (Get-Date -Format 'yyyyMMdd_HHmmss_fff')
    [IO.Directory]::CreateDirectory((Join-Path $root $dir)) | Out-Null
    Write-Output "VALIDATION_ROOT=$dir"
    function Assert([bool]$Condition,[string]$Message){if(-not $Condition){throw $Message}}
    function Tool([string]$Name,[string]$Program,[string[]]$Arguments,[int]$Expected=0) {
        $previous=$ErrorActionPreference;$ErrorActionPreference='Continue'
        try {& $Program @Arguments 2>&1 | ForEach-Object {"$_"} | Tee-Object -FilePath (Join-Path $dir ($Name+'-console.txt')) | Where-Object {$_ -notmatch '^F '};$code=$LASTEXITCODE}
        finally {$ErrorActionPreference=$previous}
        Assert ($code -eq $Expected) "$Name exit=$code, expected=$Expected"
    }
    function Summary([string]$Name){
        $data=@{};Get-Content -LiteralPath (Join-Path $dir ($Name+'\summary.txt')) -Encoding UTF8 | ForEach-Object {$pair=$_ -split '=',2;if($pair.Count -eq 2){$data[$pair[0]]=$pair[1]}};return $data
    }
    function Case([string]$Name,[string]$Source,[string[]]$Extra=@(),[switch]$Gui) {
        $a=@('--input',$Source,'--out',(Join-Path $dir $Name))
        if($Gui){$a+=@('--autostart','--close-on-end')}else{$a+='--headless'}
        Tool $Name $exe ($a+$Extra)
        $s=Summary $Name
        Assert ($s['error'] -eq '' -and $s['max_frame_packets'] -eq '2') "$Name runtime/config failure"
        Assert ([int]$s['max_sent_frame_packets'] -le 2 -and [int]$s['max_sent_frame_h264_bytes'] -le 592) "$Name sent an oversized frame"
        Assert ([long]$s['tx_data_bytes'] -eq [long]$s['packets_sent']*300) "$Name wire byte accounting mismatch"
        Tool ($Name+'-replay') $verifier @((Join-Path $dir $Name))
    }
    Tool 'protocol' $exe @('--self-test')
    $wide=Join-Path $dir '四角-横屏.mp4';$portrait=Join-Path $dir '四角-竖屏.mp4';$vfr=Join-Path $dir '变帧间隔.mp4'
    $marks='drawbox=x=0:y=0:w=48:h=48:color=red:t=fill,drawbox=x=iw-48:y=0:w=48:h=48:color=green:t=fill,drawbox=x=0:y=ih-48:w=48:h=48:color=blue:t=fill,drawbox=x=iw-48:y=ih-48:w=48:h=48:color=yellow:t=fill'
    Tool 'make-wide' $ff @('-hide_banner','-loglevel','error','-f','lavfi','-i','testsrc2=s=640x360:r=30:d=4','-vf',$marks,'-c:v','libx264','-preset','ultrafast','-y',$wide)
    Tool 'make-portrait' $ff @('-hide_banner','-loglevel','error','-f','lavfi','-i','testsrc2=s=360x640:r=30:d=4','-vf',$marks,'-c:v','libx264','-preset','ultrafast','-y',$portrait)
    Tool 'make-vfr' $ff @('-hide_banner','-loglevel','error','-f','lavfi','-i','testsrc2=s=640x360:r=20:d=3','-vf','setpts=if(lt(N\,30)\,N/(20*TB)\,(30+(N-30)*2)/(20*TB))','-fps_mode','vfr','-c:v','libx264','-preset','ultrafast','-y',$vfr)
    # One encoded bitstream, followed by lossless removal of non-image SEI.
    $before=Join-Path $dir '含编码器说明.h264';$after=Join-Path $dir '去编码器说明.h264'
    Tool 'make-sei-before' $ff @('-hide_banner','-loglevel','error','-f','rawvideo','-pixel_format','bgr24','-video_size','1280x1024','-framerate','20','-i','素材\car_raw.bgr','-map','0:v:0','-vf','scale=190:152:flags=area,pad=192:152:1:0','-frames:v','60','-c:v','libx264','-preset','slow','-tune','zerolatency','-pix_fmt','yuv420p','-threads','1','-b:v','74548','-minrate','74548','-maxrate','74548','-bufsize','4736','-g','10','-bf','0','-x264-params','repeat-headers=1:annexb=1:scenecut=0:slices=4','-f','h264','-y',$before)
    Tool 'remove-sei' $ff @('-hide_banner','-loglevel','error','-i',$before,'-map','0:v:0','-c:v','copy','-bsf:v','filter_units=remove_types=6','-f','h264','-y',$after)
    Tool 'sei-before-decode' (Join-Path $root 'build\cw_decoder_reference.exe') @($before,'0')
    Tool 'sei-after-decode' (Join-Path $root 'build\cw_decoder_reference.exe') @($after,'0')
    $a=@(Get-Content -LiteralPath (Join-Path $dir 'sei-before-decode-console.txt') -Encoding UTF8 | Where-Object {$_ -match '^(F |END )'})
    $b=@(Get-Content -LiteralPath (Join-Path $dir 'sei-after-decode-console.txt') -Encoding UTF8 | Where-Object {$_ -match '^(F |END )'})
    Assert ($a.Count -gt 1 -and $null -eq (Compare-Object $a $b)) 'SEI removal changed pixels'
    foreach($drop in @(0,20)) {
        Tool ('reference-'+$drop) (Join-Path $root 'build\cw_decoder_reference.exe') @($before,"$drop")
        Tool ('observed-'+$drop) (Join-Path $root 'build\cw_decoder_observed.exe') @($before,"$drop")
        $a=@(Get-Content -LiteralPath (Join-Path $dir ('reference-'+$drop+'-console.txt')) -Encoding UTF8 | Where-Object {$_ -match '^(F |END )'})
        $b=@(Get-Content -LiteralPath (Join-Path $dir ('observed-'+$drop+'-console.txt')) -Encoding UTF8 | Where-Object {$_ -match '^(F |END )'})
        Assert ($null -eq (Compare-Object $a $b)) 'decoder snapshot changed pixels/order'
    }
    Case 'wide' $wide
    $s=Summary 'wide';Assert ([int]$s['source_frames'] -eq 120 -and [int]$s['sampling_skipped'] -eq 40 -and ([int]$s['encoded']+[int]$s['encode_overwrite']) -eq 80) 'source PTS sampling mismatch'
    Case 'portrait' $portrait
    Case 'vfr' $vfr
    Case 'raw' '素材\car_raw.bgr' @('--duration','4')
    Case 'loss' $wide @('--drop-packet','20')
    Case 'all-drop' $wide @('--test-drop-all') -Gui
    $s=Summary 'all-drop';Assert ([int]$s['source_frames'] -gt 0 -and [int]$s['packets_sent'] -gt 0 -and [int]$s['rx_packets'] -eq 0 -and [int]$s['decoded'] -eq 0 -and [int]$s['displayed_or_headless_consumed'] -eq 0) 'image bypass when all packets were lost'
    Assert (-not (Test-Path -LiteralPath (Join-Path $dir 'all-drop\last-received.png'))) 'received image exists without received packets'
    Case 'oversize-idr' '素材\car_raw.bgr' @('--duration','4','--test-oversize-frame','1')
    $s=Summary 'oversize-idr';Assert ([int]$s['oversize'] -eq 1 -and [int]$s['decoded'] -gt 0) 'oversize IDR rejection/recovery failed'
    $sent=Import-Csv -LiteralPath (Join-Path $dir 'oversize-idr\tx-times.csv');Assert (@($sent|Where-Object source_frame -eq '1').Count -eq 0) 'oversized keyframe was transmitted'
    foreach($part in @('pre','encode','post')){Case ($part+'-load') $wide @(('--test-'+$part+'-ms'),'100')}
    $base=Summary 'wide';$pre=Summary 'pre-load';$enc=Summary 'encode-load';$post=Summary 'post-load';$loss=Summary 'loss'
    Assert ([int]$pre['encode_overwrite'] -gt 0 -and [int]$enc['encode_overwrite'] -gt 0) 'real processing load did not overwrite pending frames'
    Assert ([int]$pre['source_frames'] -eq 120 -and [int]$enc['source_frames'] -eq 120 -and [int]$post['source_frames'] -eq 120) 'load slowed source playback'
    Assert ([double]$post['latency_median_ms'] -gt [double]$base['latency_median_ms']+100) 'postprocessing time missing from latency'
    Assert ([int]$loss['network_drop'] -eq 1 -and [int]$loss['decoded_unassociated'] -eq 0) 'loss recovery/association failed'
    $ev=Import-Csv -LiteralPath (Join-Path $dir 'wide\events.csv')
    $first=[double]($ev | Where-Object event -eq 'headless-consumed' | Select-Object -First 1).time_ms
    $last=[double]($ev | Where-Object event -eq 'source' | Select-Object -Last 1).time_ms
    Assert ($first -gt 0 -and $first -lt $last) 'not genuinely online streaming'
    Case 'fps25' $wide @('--fps','25')
    Tool 'over-fps' $exe @('--headless','--fps','26') 2
    Tool 'over-rate' $exe @('--headless','--send-rate','51') 2
    Tool 'odd-size' $exe @('--headless','--size','191x152') 2
    Tool 'missing-file' $exe @('--headless','--input',(Join-Path $dir 'missing.mp4'),'--out',(Join-Path $dir 'missing')) 2
    $broken=Join-Path $dir '残缺.bgr';[IO.File]::WriteAllBytes((Join-Path $root $broken),[byte[]](1,2,3))
    Tool 'broken-raw' $exe @('--headless','--input',$broken,'--out',(Join-Path $dir 'broken')) 2
    # Detached proof has no original video or trace records. Corrupt a packet length header to reject it.
    $detached=Join-Path $dir 'detached-proof';[IO.Directory]::CreateDirectory((Join-Path $root $detached))|Out-Null
    foreach($file in @('rx-times.csv','rx-300.bin','decoded.csv')){Copy-Item -LiteralPath (Join-Path $dir ('wide\'+$file)) -Destination (Join-Path $detached $file)}
    Tool 'detached-proof' $verifier @('--rx-only',$detached)
    $corrupt=Join-Path $dir 'tampered-proof';[IO.Directory]::CreateDirectory((Join-Path $root $corrupt))|Out-Null
    foreach($file in @('rx-times.csv','rx-300.bin','decoded.csv')){Copy-Item -LiteralPath (Join-Path $detached $file) -Destination (Join-Path $corrupt $file)}
    $tampered=Join-Path $root (Join-Path $corrupt 'rx-300.bin');$bytes=[IO.File]::ReadAllBytes($tampered);$bytes[0]=255;[IO.File]::WriteAllBytes($tampered,$bytes)
    Tool 'tampered-proof' $verifier @('--rx-only',$corrupt) 1
    if($Full){
        Case 'gui60' '素材\car_raw.bgr' @('--loop','--duration','60') -Gui
        $s=Summary 'gui60';Assert ($s['display_acceptance'] -eq 'PASS' -and [int]$s['minimum_stable_display_fps'] -ge 15 -and [int]$s['display_windows_below_15'] -eq 0 -and [int]$s['stable_display_windows'] -ge 50) 'actual display did not meet 15 fps in each steady second'
        Assert ([int]$s['decoded_unassociated'] -eq 0 -and [int]$s['max_trace_records'] -le 512) 'trace association/bounds failure'
    }
    Write-Output "PASS two-packet realtime and independent wire proof: $dir"
} finally {Pop-Location}
