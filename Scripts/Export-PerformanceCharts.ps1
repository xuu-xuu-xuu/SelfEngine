param([Parameter(Mandatory=$true)][string]$Data,[Parameter(Mandatory=$true)][string]$OutputDirectory)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Windows.Forms.DataVisualization
Add-Type -AssemblyName System.Drawing
[Threading.Thread]::CurrentThread.CurrentCulture=[Globalization.CultureInfo]::InvariantCulture
$document=[IO.File]::ReadAllText((Resolve-Path -LiteralPath $Data).Path,[Text.Encoding]::UTF8)|ConvertFrom-Json
if($document.schema -ne 1){throw 'Unsupported chart data schema.'}
$output=[IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Path $output -Force | Out-Null
foreach($description in $document.charts){
    if([IO.Path]::GetFileName($description.file) -ne $description.file -or -not $description.file.EndsWith('.png')){throw 'Invalid chart output name.'}
    $plot=New-Object System.Windows.Forms.DataVisualization.Charting.Chart
    try{
        $plot.Width=1200;$plot.Height=700;$plot.BackColor=[Drawing.Color]::White
        $plot.AntiAliasing=[Windows.Forms.DataVisualization.Charting.AntiAliasingStyles]::All
        $plot.TextAntiAliasingQuality=[Windows.Forms.DataVisualization.Charting.TextAntiAliasingQuality]::High
        $area=New-Object Windows.Forms.DataVisualization.Charting.ChartArea 'plot'
        $area.BackColor=[Drawing.Color]::White
        $area.AxisX.Interval=1;$area.AxisX.MajorGrid.Enabled=$false
        $area.AxisX.LineColor=$area.AxisY.LineColor=[Drawing.Color]::FromArgb(130,140,150)
        $area.AxisX.LabelStyle.Font=[Drawing.Font]::new('Microsoft YaHei',[float]12)
        $area.AxisY.LabelStyle.Font=[Drawing.Font]::new('Microsoft YaHei',[float]11)
        $area.AxisY.Title=$description.unit;$area.AxisY.TitleFont=[Drawing.Font]::new('Microsoft YaHei',[float]12)
        $area.AxisY.Minimum=0;$area.AxisY.MajorGrid.LineColor=[Drawing.Color]::FromArgb(225,230,235)
        [void]$plot.ChartAreas.Add($area)
        $legend=New-Object Windows.Forms.DataVisualization.Charting.Legend 'legend'
        $legend.Docking=[Windows.Forms.DataVisualization.Charting.Docking]::Bottom
        $legend.Font=[Drawing.Font]::new('Microsoft YaHei',[float]11)
        [void]$plot.Legends.Add($legend)
        $title=New-Object Windows.Forms.DataVisualization.Charting.Title
        $title.Text=$description.title;$title.Font=[Drawing.Font]::new('Microsoft YaHei',[float]19,[Drawing.FontStyle]::Bold)
        [void]$plot.Titles.Add($title)
        $caption=New-Object Windows.Forms.DataVisualization.Charting.Title
        $caption.Text=$description.caption;$caption.Font=[Drawing.Font]::new('Microsoft YaHei',[float]10)
        $caption.Docking=[Windows.Forms.DataVisualization.Charting.Docking]::Bottom
        [void]$plot.Titles.Add($caption)
        $maximum=0.0
        foreach($line in $description.series){
            if($line.points.Count -ne $description.categories.Count){throw 'Chart category/point mismatch.'}
            $series=New-Object Windows.Forms.DataVisualization.Charting.Series $line.name
            $series.ChartType=[Windows.Forms.DataVisualization.Charting.SeriesChartType]::Column
            $series.Color=[Drawing.ColorTranslator]::FromHtml($line.color)
            $series.Font=[Drawing.Font]::new('Microsoft YaHei',[float]11)
            $series['PointWidth']='0.62';$series['PixelPointDepth']='0'
            for($index=0;$index -lt $line.points.Count;$index++){
                $point=$line.points[$index];$value=[double]$point.mean;$spread=[double]$point.stddev
                if([double]::IsNaN($value) -or [double]::IsInfinity($value) -or $value -lt 0 -or $spread -lt 0){throw 'Invalid chart measurement.'}
                $position=$series.Points.AddXY([string]$description.categories[$index],$value)
                $series.Points[$position].Label=$value.ToString('0.000')+"`n"+[char]0x00b1+$spread.ToString('0.000')
                $maximum=[Math]::Max($maximum,$value+$spread)
            }
            [void]$plot.Series.Add($series)
        }
        $area.AxisY.Maximum=[Math]::Max(1,$maximum*1.25)
        $plot.SaveImage((Join-Path $output $description.file),[Windows.Forms.DataVisualization.Charting.ChartImageFormat]::Png)
    }finally{$plot.Dispose()}
}
Write-Output ('Exported '+$document.charts.Count+' charts using System.Windows.Forms.DataVisualization: '+$output)
