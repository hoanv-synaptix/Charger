def create_xaml():
    return """<Window x:Class="ChargerDebugApp.MainWindow"
        xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
        xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
        Title="PKG Battery - Charger Debug App - UI v3 BMS (WPF Port)" Height="900" Width="1400"
        Background="#E2E8F0" WindowStartupLocation="CenterScreen" FontFamily="Segoe UI">
    
    <Window.Resources>
        <!-- Engineering Style GroupBox: Clear boundaries and distinct headers -->
        <Style TargetType="GroupBox">
            <Setter Property="Margin" Value="6"/>
            <Setter Property="Padding" Value="8"/>
            <Setter Property="Background" Value="White"/>
            <Setter Property="BorderBrush" Value="#94A3B8"/>
            <Setter Property="BorderThickness" Value="1"/>
            <Setter Property="Template">
                <Setter.Value>
                    <ControlTemplate TargetType="GroupBox">
                        <Border Background="{TemplateBinding Background}" BorderBrush="{TemplateBinding BorderBrush}" BorderThickness="{TemplateBinding BorderThickness}" CornerRadius="4">
                            <Grid>
                                <Grid.RowDefinitions>
                                    <RowDefinition Height="Auto"/>
                                    <RowDefinition Height="*"/>
                                </Grid.RowDefinitions>
                                <Border Grid.Row="0" Background="#F1F5F9" BorderBrush="#94A3B8" BorderThickness="0,0,0,1" CornerRadius="4,4,0,0" Padding="10,6">
                                    <ContentPresenter ContentSource="Header" TextElement.FontWeight="Bold" TextElement.Foreground="#0F172A" TextElement.FontSize="13"/>
                                </Border>
                                <ContentPresenter Grid.Row="1" Margin="{TemplateBinding Padding}"/>
                            </Grid>
                        </Border>
                    </ControlTemplate>
                </Setter.Value>
            </Setter>
        </Style>

        <!-- Strong Input Styles -->
        <Style TargetType="TextBox">
            <Setter Property="Margin" Value="4,4,10,4"/>
            <Setter Property="Padding" Value="6,4"/>
            <Setter Property="BorderBrush" Value="#64748B"/>
            <Setter Property="BorderThickness" Value="1"/>
            <Setter Property="Background" Value="#FFFFFF"/>
            <Setter Property="VerticalContentAlignment" Value="Center"/>
            <Style.Triggers>
                <Trigger Property="IsFocused" Value="True">
                    <Setter Property="BorderBrush" Value="#2563EB"/>
                    <Setter Property="BorderThickness" Value="2"/>
                </Trigger>
            </Style.Triggers>
        </Style>

        <Style TargetType="ComboBox">
            <Setter Property="Margin" Value="4,4,10,4"/>
            <Setter Property="Padding" Value="6,4"/>
            <Setter Property="BorderBrush" Value="#64748B"/>
            <Setter Property="BorderThickness" Value="1"/>
        </Style>

        <!-- Labels -->
        <Style TargetType="TextBlock" x:Key="LabelStyle">
            <Setter Property="VerticalAlignment" Value="Center"/>
            <Setter Property="Foreground" Value="#334155"/>
            <Setter Property="FontWeight" Value="SemiBold"/>
            <Setter Property="Margin" Value="4,0,8,0"/>
        </Style>

        <Style TargetType="TextBlock" x:Key="ValueStyle">
            <Setter Property="VerticalAlignment" Value="Center"/>
            <Setter Property="Foreground" Value="#0F172A"/>
            <Setter Property="FontWeight" Value="Bold"/>
            <Setter Property="FontSize" Value="14"/>
        </Style>

        <!-- Buttons with solid borders -->
        <Style TargetType="Button">
            <Setter Property="Background" Value="#F8FAFC"/>
            <Setter Property="Foreground" Value="#0F172A"/>
            <Setter Property="BorderBrush" Value="#94A3B8"/>
            <Setter Property="BorderThickness" Value="1"/>
            <Setter Property="Padding" Value="12,6"/>
            <Setter Property="Margin" Value="4"/>
            <Setter Property="FontWeight" Value="SemiBold"/>
            <Setter Property="Template">
                <Setter.Value>
                    <ControlTemplate TargetType="Button">
                        <Border Background="{TemplateBinding Background}" BorderBrush="{TemplateBinding BorderBrush}" BorderThickness="{TemplateBinding BorderThickness}" CornerRadius="3">
                            <ContentPresenter HorizontalAlignment="Center" VerticalAlignment="Center"/>
                        </Border>
                    </ControlTemplate>
                </Setter.Value>
            </Setter>
            <Style.Triggers>
                <Trigger Property="IsMouseOver" Value="True">
                    <Setter Property="Background" Value="#E2E8F0"/>
                </Trigger>
            </Style.Triggers>
        </Style>
        
        <!-- Primary Action Button -->
        <Style TargetType="Button" x:Key="PrimaryButton" BasedOn="{StaticResource {x:Type Button}}">
            <Setter Property="Background" Value="#2563EB"/>
            <Setter Property="Foreground" Value="White"/>
            <Setter Property="BorderBrush" Value="#1D4ED8"/>
            <Style.Triggers>
                <Trigger Property="IsMouseOver" Value="True">
                    <Setter Property="Background" Value="#1D4ED8"/>
                </Trigger>
            </Style.Triggers>
        </Style>
    </Window.Resources>

    <Grid>
        <Grid.RowDefinitions>
            <RowDefinition Height="Auto"/>
            <RowDefinition Height="*"/>
            <RowDefinition Height="Auto"/>
        </Grid.RowDefinitions>

        <!-- CONNECTION BAR (Explicitly framed) -->
        <Border Grid.Row="0" Background="White" BorderBrush="#94A3B8" BorderThickness="0,0,0,2" Padding="10,6">
            <StackPanel Orientation="Horizontal" VerticalAlignment="Center">
                <TextBlock Text="Port:" Style="{StaticResource LabelStyle}"/>
                <ComboBox Width="120"/>
                <Button Content="Refresh"/>
                <Button Content="Connect" Style="{StaticResource PrimaryButton}" Width="90"/>
                
                <Rectangle Width="2" Fill="#E2E8F0" Margin="15,0"/>
                <TextBlock Text="Baudrate:" Style="{StaticResource LabelStyle}"/>
                <TextBlock Text="115200" Style="{StaticResource ValueStyle}" Margin="0,0,15,0"/>
                
                <Rectangle Width="2" Fill="#E2E8F0" Margin="15,0"/>
                <TextBlock Text="Disconnected" Foreground="#EF4444" FontWeight="Bold" VerticalAlignment="Center" Margin="5,0"/>
                
                <CheckBox Content="Serial Debug Log" Margin="30,0,0,0" VerticalAlignment="Center" Foreground="#475569" FontWeight="SemiBold"/>
            </StackPanel>
        </Border>

        <!-- MAIN TABS -->
        <TabControl Grid.Row="1" Margin="8" Background="#F8FAFC" BorderBrush="#94A3B8" BorderThickness="1">
            
            <!-- CONTROL TAB -->
            <TabItem Header="Control">
                <Grid Margin="5">
                    <Grid.ColumnDefinitions>
                        <ColumnDefinition Width="340"/>
                        <ColumnDefinition Width="*"/>
                    </Grid.ColumnDefinitions>
                    
                    <!-- Left Panel -->
                    <StackPanel Grid.Column="0">
                        <GroupBox Header="Module Management">
                            <StackPanel>
                                <Border Background="#F1F5F9" BorderBrush="#CBD5E1" BorderThickness="1" CornerRadius="3" Padding="8" Margin="0,0,0,10">
                                    <StackPanel>
                                        <StackPanel Orientation="Horizontal" Margin="0,0,0,8">
                                            <TextBlock Text="Address:" Width="60" Style="{StaticResource LabelStyle}"/>
                                            <TextBox Text="0x01" Width="100"/>
                                            <Button Content="Add" Style="{StaticResource PrimaryButton}"/>
                                        </StackPanel>
                                        <StackPanel Orientation="Horizontal">
                                            <TextBlock Text="Driver:" Width="60" Style="{StaticResource LabelStyle}"/>
                                            <ComboBox Width="140" SelectedIndex="0">
                                                <ComboBoxItem>Maxwell</ComboBoxItem>
                                                <ComboBoxItem>Lianming</ComboBoxItem>
                                                <ComboBoxItem>TonHe</ComboBoxItem>
                                            </ComboBox>
                                        </StackPanel>
                                    </StackPanel>
                                </Border>
                                
                                <DataGrid Height="120" AutoGenerateColumns="False" HeadersVisibility="Column" Background="White" BorderBrush="#94A3B8" BorderThickness="1" Margin="0,0,0,10">
                                    <DataGrid.Columns>
                                        <DataGridTextColumn Header="Addr" Width="50"/>
                                        <DataGridTextColumn Header="Driver" Width="80"/>
                                        <DataGridTextColumn Header="Online" Width="*"/>
                                    </DataGrid.Columns>
                                </DataGrid>
                                
                                <UniformGrid Columns="2">
                                    <Button Content="Remove"/>
                                    <Button Content="Clear"/>
                                </UniformGrid>
                            </StackPanel>
                        </GroupBox>

                        <GroupBox Header="Commands">
                            <StackPanel>
                                <CheckBox Content="Manual Mode" Margin="0,0,0,10" FontWeight="Bold" Foreground="#DC2626"/>
                                <Border Background="#FFF1F2" BorderBrush="#FECDD3" BorderThickness="1" CornerRadius="3" Padding="8" Margin="0,0,0,10">
                                    <StackPanel>
                                        <StackPanel Orientation="Horizontal" Margin="0,0,0,5">
                                            <TextBlock Text="Voltage:" Width="60" Style="{StaticResource LabelStyle}"/>
                                            <TextBox Text="54.6" Width="90"/>
                                            <Button Content="Set V"/>
                                        </StackPanel>
                                        <StackPanel Orientation="Horizontal">
                                            <TextBlock Text="Current:" Width="60" Style="{StaticResource LabelStyle}"/>
                                            <TextBox Text="20.0" Width="90"/>
                                            <Button Content="Set I"/>
                                        </StackPanel>
                                    </StackPanel>
                                </Border>
                                <Button Content="▶ START (F5)" Background="#10B981" Foreground="White" BorderBrush="#059669" FontWeight="Bold" Margin="0,5,0,5" Height="44"/>
                                <Button Content="⏹ STOP (F6)" Background="#F59E0B" Foreground="White" BorderBrush="#D97706" FontWeight="Bold" Margin="0,0,0,5" Height="44"/>
                                <Button Content="⚠️ EMERGENCY STOP (F9)" Background="#EF4444" Foreground="White" BorderBrush="#B91C1C" FontWeight="Bold" Margin="0,10,0,0" Height="44"/>
                            </StackPanel>
                        </GroupBox>
                    </StackPanel>
                    
                    <!-- Right Panel -->
                    <Grid Grid.Column="1" Margin="10,0,0,0">
                        <Grid.ColumnDefinitions>
                            <ColumnDefinition Width="1.5*"/>
                            <ColumnDefinition Width="1*"/>
                        </Grid.ColumnDefinitions>

                        <GroupBox Grid.Column="0" Header="Selected Module Focus">
                            <StackPanel>
                                <Border Background="#F8FAFC" BorderBrush="#CBD5E1" BorderThickness="1" CornerRadius="3" Padding="12" Margin="0,0,0,10">
                                    <StackPanel>
                                        <TextBlock Text="State and Identity" Foreground="#0F172A" FontWeight="Bold" Margin="0,0,0,10"/>
                                        <UniformGrid Columns="2" Margin="0,0,0,10">
                                            <StackPanel Orientation="Horizontal"><TextBlock Text="Driver:" Style="{StaticResource LabelStyle}"/><TextBlock Text="---" Style="{StaticResource ValueStyle}"/></StackPanel>
                                            <StackPanel Orientation="Horizontal"><TextBlock Text="State:" Style="{StaticResource LabelStyle}"/><TextBlock Text="---" Style="{StaticResource ValueStyle}"/></StackPanel>
                                            <StackPanel Orientation="Horizontal"><TextBlock Text="Online:" Style="{StaticResource LabelStyle}"/><TextBlock Text="---" Style="{StaticResource ValueStyle}"/></StackPanel>
                                            <StackPanel Orientation="Horizontal"><TextBlock Text="Running:" Style="{StaticResource LabelStyle}"/><TextBlock Text="---" Style="{StaticResource ValueStyle}"/></StackPanel>
                                        </UniformGrid>
                                    </StackPanel>
                                </Border>
                                
                                <Border Background="#F0FDF4" BorderBrush="#BBF7D0" BorderThickness="1" CornerRadius="3" Padding="12">
                                    <StackPanel>
                                        <TextBlock Text="Electrical Output" Foreground="#166534" FontWeight="Bold" Margin="0,0,0,10"/>
                                        <UniformGrid Columns="2">
                                            <StackPanel Orientation="Horizontal"><TextBlock Text="Voltage:" Style="{StaticResource LabelStyle}"/><TextBlock Text="--- V" Style="{StaticResource ValueStyle}" Foreground="#166534" FontSize="18"/></StackPanel>
                                            <StackPanel Orientation="Horizontal"><TextBlock Text="Current:" Style="{StaticResource LabelStyle}"/><TextBlock Text="--- A" Style="{StaticResource ValueStyle}" Foreground="#166534" FontSize="18"/></StackPanel>
                                            <StackPanel Orientation="Horizontal" Margin="0,10,0,0"><TextBlock Text="Limit:" Style="{StaticResource LabelStyle}"/><TextBlock Text="--- A" Style="{StaticResource ValueStyle}"/></StackPanel>
                                        </UniformGrid>
                                    </StackPanel>
                                </Border>
                            </StackPanel>
                        </GroupBox>
                        
                        <GroupBox Grid.Column="1" Header="Operator Guide">
                            <StackPanel>
                                <TextBlock Text="1. Pick the target module first." TextWrapping="Wrap" Margin="0,8" Foreground="#334155"/>
                                <TextBlock Text="2. Use Monitor to verify actions." TextWrapping="Wrap" Margin="0,8" Foreground="#334155"/>
                                <TextBlock Text="3. Use Charge Config for protection limits." TextWrapping="Wrap" Margin="0,8" Foreground="#334155"/>
                                <TextBlock Text="4. Emergency Stop is global." TextWrapping="Wrap" Margin="0,8" Foreground="#DC2626" FontWeight="Bold"/>
                            </StackPanel>
                        </GroupBox>
                    </Grid>
                </Grid>
            </TabItem>

            <!-- MONITOR TAB -->
            <TabItem Header="Monitor">
                <Grid Margin="5">
                    <Grid.ColumnDefinitions>
                        <ColumnDefinition Width="4*"/>
                        <ColumnDefinition Width="3*"/>
                    </Grid.ColumnDefinitions>
                    
                    <GroupBox Grid.Column="0" Header="Charger Monitor">
                        <Grid>
                            <Grid.RowDefinitions>
                                <RowDefinition Height="Auto"/>
                                <RowDefinition Height="*"/>
                            </Grid.RowDefinitions>
                            <DataGrid Grid.Row="0" Height="200" AutoGenerateColumns="False" HeadersVisibility="Column" Background="White" BorderBrush="#94A3B8" BorderThickness="1">
                                <DataGrid.Columns>
                                    <DataGridTextColumn Header="Addr" Width="70"/>
                                    <DataGridTextColumn Header="Driver" Width="90"/>
                                    <DataGridTextColumn Header="Online" Width="70"/>
                                </DataGrid.Columns>
                            </DataGrid>
                            <GroupBox Grid.Row="1" Header="Module Detail" Margin="0,10,0,0">
                                <UniformGrid Columns="4">
                                    <StackPanel HorizontalAlignment="Center"><TextBlock Text="Voltage" Style="{StaticResource LabelStyle}" HorizontalAlignment="Center"/><TextBlock Text="--- V" Style="{StaticResource ValueStyle}" FontSize="24" Foreground="#2563EB"/></StackPanel>
                                    <StackPanel HorizontalAlignment="Center"><TextBlock Text="Current" Style="{StaticResource LabelStyle}" HorizontalAlignment="Center"/><TextBlock Text="--- A" Style="{StaticResource ValueStyle}" FontSize="24" Foreground="#2563EB"/></StackPanel>
                                    <StackPanel HorizontalAlignment="Center"><TextBlock Text="Power" Style="{StaticResource LabelStyle}" HorizontalAlignment="Center"/><TextBlock Text="--- kW" Style="{StaticResource ValueStyle}" FontSize="24"/></StackPanel>
                                    <StackPanel HorizontalAlignment="Center"><TextBlock Text="Temp" Style="{StaticResource LabelStyle}" HorizontalAlignment="Center"/><TextBlock Text="--- °C" Style="{StaticResource ValueStyle}" FontSize="24"/></StackPanel>
                                </UniformGrid>
                            </GroupBox>
                        </Grid>
                    </GroupBox>

                    <GroupBox Grid.Column="1" Header="BMS Monitor">
                        <StackPanel>
                            <GroupBox Header="BMS Snapshot">
                                <UniformGrid Columns="2" Margin="0,10">
                                    <StackPanel Orientation="Horizontal" Margin="0,10"><TextBlock Text="Pack Voltage:" Style="{StaticResource LabelStyle}"/><TextBlock Text="--- V" Style="{StaticResource ValueStyle}"/></StackPanel>
                                    <StackPanel Orientation="Horizontal" Margin="0,10"><TextBlock Text="Pack Current:" Style="{StaticResource LabelStyle}"/><TextBlock Text="--- A" Style="{StaticResource ValueStyle}"/></StackPanel>
                                    <StackPanel Orientation="Horizontal" Margin="0,10"><TextBlock Text="SOC:" Style="{StaticResource LabelStyle}"/><TextBlock Text="--- %" Style="{StaticResource ValueStyle}" Foreground="#10B981"/></StackPanel>
                                    <StackPanel Orientation="Horizontal" Margin="0,10"><TextBlock Text="SOH:" Style="{StaticResource LabelStyle}"/><TextBlock Text="--- %" Style="{StaticResource ValueStyle}"/></StackPanel>
                                </UniformGrid>
                            </GroupBox>
                            <GroupBox Header="BMS Metrics">
                                <UniformGrid Columns="2" Margin="0,10">
                                    <StackPanel Orientation="Horizontal" Margin="0,10"><TextBlock Text="Max Cell V:" Style="{StaticResource LabelStyle}"/><TextBlock Text="--- V" Style="{StaticResource ValueStyle}"/></StackPanel>
                                    <StackPanel Orientation="Horizontal" Margin="0,10"><TextBlock Text="Min Cell V:" Style="{StaticResource LabelStyle}"/><TextBlock Text="--- V" Style="{StaticResource ValueStyle}"/></StackPanel>
                                    <StackPanel Orientation="Horizontal" Margin="0,10"><TextBlock Text="Max Temp:" Style="{StaticResource LabelStyle}"/><TextBlock Text="--- °C" Style="{StaticResource ValueStyle}"/></StackPanel>
                                    <StackPanel Orientation="Horizontal" Margin="0,10"><TextBlock Text="Min Temp:" Style="{StaticResource LabelStyle}"/><TextBlock Text="--- °C" Style="{StaticResource ValueStyle}"/></StackPanel>
                                </UniformGrid>
                            </GroupBox>
                        </StackPanel>
                    </GroupBox>
                </Grid>
            </TabItem>

            <!-- CHARGE CONFIG TAB -->
            <TabItem Header="Charge Config">
                <Grid Margin="5">
                    <Grid.RowDefinitions>
                        <RowDefinition Height="Auto"/>
                        <RowDefinition Height="*"/>
                    </Grid.RowDefinitions>
                    
                    <Border Grid.Row="0" Background="White" BorderBrush="#94A3B8" BorderThickness="1" CornerRadius="4" Padding="10" Margin="0,0,0,10">
                        <StackPanel Orientation="Horizontal">
                            <TextBlock Text="Charge Cycle Configuration" Foreground="#0F172A" FontWeight="Black" FontSize="18" Margin="0,0,30,0"/>
                            <Button Content="Import"/>
                            <Button Content="Export"/>
                            <Button Content="Read MCU" Style="{StaticResource PrimaryButton}"/>
                            <Button Content="Write MCU" Background="#10B981" Foreground="White" BorderBrush="#059669"/>
                            <Button Content="Defaults"/>
                        </StackPanel>
                    </Border>

                    <ScrollViewer Grid.Row="1" VerticalScrollBarVisibility="Auto">
                        <Grid>
                            <Grid.ColumnDefinitions>
                                <ColumnDefinition Width="4*"/>
                                <ColumnDefinition Width="3*"/>
                            </Grid.ColumnDefinitions>

                            <StackPanel Grid.Column="0">
                                <GroupBox Header="General Limits">
                                    <StackPanel>
                                        <GroupBox Header="Pack Parameters">
                                            <UniformGrid Columns="4">
                                                <StackPanel Margin="0,5"><TextBlock Text="Battery Cap (Ah)" Style="{StaticResource LabelStyle}"/><TextBox Text="0"/></StackPanel>
                                                <StackPanel Margin="0,5"><TextBlock Text="Temp Limit (C)" Style="{StaticResource LabelStyle}"/><TextBox Text="0"/></StackPanel>
                                                <StackPanel Margin="0,5"><TextBlock Text="V Min (V)" Style="{StaticResource LabelStyle}"/><TextBox Text="0"/></StackPanel>
                                                <StackPanel Margin="0,5"><TextBlock Text="V Max (V)" Style="{StaticResource LabelStyle}"/><TextBox Text="0"/></StackPanel>
                                                <StackPanel Margin="0,5"><TextBlock Text="I Min (C)" Style="{StaticResource LabelStyle}"/><TextBox Text="0"/></StackPanel>
                                                <StackPanel Margin="0,5"><TextBlock Text="I Max (C)" Style="{StaticResource LabelStyle}"/><TextBox Text="0"/></StackPanel>
                                            </UniformGrid>
                                        </GroupBox>
                                        <GroupBox Header="Charge Window">
                                            <UniformGrid Columns="4">
                                                <StackPanel Margin="0,5"><TextBlock Text="V Precharge (V)" Style="{StaticResource LabelStyle}"/><TextBox Text="0"/></StackPanel>
                                                <StackPanel Margin="0,5"><TextBlock Text="V Low (V)" Style="{StaticResource LabelStyle}"/><TextBox Text="0"/></StackPanel>
                                                <StackPanel Margin="0,5"><TextBlock Text="I Precharge (C)" Style="{StaticResource LabelStyle}"/><TextBox Text="0"/></StackPanel>
                                                <StackPanel Margin="0,5"><TextBlock Text="I Low (C)" Style="{StaticResource LabelStyle}"/><TextBox Text="0"/></StackPanel>
                                            </UniformGrid>
                                        </GroupBox>
                                    </StackPanel>
                                </GroupBox>

                                <GroupBox Header="Charging Strategy">
                                    <StackPanel>
                                        <!-- Will be injected -->
                                        [STAGES_PLACEHOLDER]
                                    </StackPanel>
                                </GroupBox>
                            </StackPanel>

                            <StackPanel Grid.Column="1">
                                <GroupBox Header="Protection and Hardware">
                                    <StackPanel>
                                        <GroupBox Header="System Mapping">
                                            <UniformGrid Columns="2">
                                                <StackPanel Margin="0,5"><TextBlock Text="BMS CAN ID:" Style="{StaticResource LabelStyle}"/><TextBox Text="0x1801F456"/></StackPanel>
                                                <StackPanel Margin="0,5"><TextBlock Text="Charge Source:" Style="{StaticResource LabelStyle}"/>
                                                    <ComboBox SelectedIndex="0">
                                                        <ComboBoxItem>CAN Override</ComboBoxItem>
                                                        <ComboBoxItem>Manual Override</ComboBoxItem>
                                                        <ComboBoxItem>Auto (BMS)</ComboBoxItem>
                                                    </ComboBox>
                                                </StackPanel>
                                            </UniformGrid>
                                        </GroupBox>
                                        
                                        <GroupBox Header="Charge Jack Protection">
                                            <UniformGrid Columns="2">
                                                <CheckBox Content="Enabled" VerticalAlignment="Center" FontWeight="SemiBold" Margin="5,0"/> <TextBlock Text=""/>
                                                <StackPanel Margin="0,5"><TextBlock Text="Delta V (V):" Style="{StaticResource LabelStyle}"/><TextBox Text="0"/></StackPanel>
                                                <StackPanel Margin="0,5"><TextBlock Text="Delay (s):" Style="{StaticResource LabelStyle}"/><TextBox Text="0"/></StackPanel>
                                            </UniformGrid>
                                        </GroupBox>

                                        <GroupBox Header="Jack Temperature Protection">
                                            <UniformGrid Columns="2">
                                                <CheckBox Content="Enabled" VerticalAlignment="Center" FontWeight="SemiBold" Margin="5,0"/> <TextBlock Text=""/>
                                                <StackPanel Margin="0,5"><TextBlock Text="Threshold (C):" Style="{StaticResource LabelStyle}"/><TextBox Text="0"/></StackPanel>
                                                <StackPanel Margin="0,5"><TextBlock Text="Delta (C):" Style="{StaticResource LabelStyle}"/><TextBox Text="0"/></StackPanel>
                                                <StackPanel Margin="0,5"><TextBlock Text="Delay (s):" Style="{StaticResource LabelStyle}"/><TextBox Text="0"/></StackPanel>
                                                <StackPanel Margin="0,5"><TextBlock Text="Power Limit (%):" Style="{StaticResource LabelStyle}"/><TextBox Text="0"/></StackPanel>
                                            </UniformGrid>
                                        </GroupBox>

                                        <GroupBox Header="Charger Module Envelope">
                                            <UniformGrid Columns="2">
                                                <StackPanel Margin="0,5"><TextBlock Text="Module Type:" Style="{StaticResource LabelStyle}"/>
                                                    <ComboBox SelectedIndex="0">
                                                        <ComboBoxItem>Type A</ComboBoxItem>
                                                        <ComboBoxItem>Type B</ComboBoxItem>
                                                    </ComboBox>
                                                </StackPanel>
                                                <StackPanel Margin="0,5"><TextBlock Text="Source Modules:" Style="{StaticResource LabelStyle}"/><TextBox Text="2"/></StackPanel>
                                                <StackPanel Margin="0,5"><TextBlock Text="U Min (V):" Style="{StaticResource LabelStyle}"/><TextBox Text="0"/></StackPanel>
                                                <StackPanel Margin="0,5"><TextBlock Text="U Max (V):" Style="{StaticResource LabelStyle}"/><TextBox Text="0"/></StackPanel>
                                                <StackPanel Margin="0,5"><TextBlock Text="I Min (A):" Style="{StaticResource LabelStyle}"/><TextBox Text="0"/></StackPanel>
                                                <StackPanel Margin="0,5"><TextBlock Text="I Max (A):" Style="{StaticResource LabelStyle}"/><TextBox Text="0"/></StackPanel>
                                            </UniformGrid>
                                        </GroupBox>
                                    </StackPanel>
                                </GroupBox>
                            </StackPanel>
                        </Grid>
                    </ScrollViewer>
                </Grid>
            </TabItem>
            <TabItem Header="Charge Graph"/>
        </TabControl>
        <Border Grid.Row="2" Background="#1E293B" Padding="10,6">
            <StackPanel Orientation="Horizontal">
                <TextBlock Text="Ready" Foreground="White" FontWeight="SemiBold"/>
            </StackPanel>
        </Border>
    </Grid>
</Window>"""

def get_stage_xaml(title, unit_t, unit_c, t_prefix, c_prefix):
    return f"""
                                        <GroupBox Header="{title}">
                                            <StackPanel>
                                                <Border Background="#F8FAFC" BorderBrush="#E2E8F0" BorderThickness="1" CornerRadius="4" Padding="10" Margin="0,0,0,10">
                                                    <StackPanel Orientation="Horizontal">
                                                        <CheckBox Content="Enabled" Margin="0,0,30,0" FontWeight="SemiBold" VerticalAlignment="Center"/>
                                                        <TextBlock Text="Delta ({unit_t}):" Style="{{StaticResource LabelStyle}}"/>
                                                        <TextBox Text="0" Width="80"/>
                                                    </StackPanel>
                                                </Border>
                                                
                                                <Grid Margin="0,5">
                                                    <Grid.ColumnDefinitions>
                                                        <ColumnDefinition Width="100"/>
                                                        <ColumnDefinition Width="*"/>
                                                        <ColumnDefinition Width="*"/>
                                                        <ColumnDefinition Width="*"/>
                                                        <ColumnDefinition Width="*"/>
                                                        <ColumnDefinition Width="*"/>
                                                    </Grid.ColumnDefinitions>
                                                    <Grid.RowDefinitions>
                                                        <RowDefinition Height="Auto"/>
                                                        <RowDefinition Height="Auto"/>
                                                        <RowDefinition Height="Auto"/>
                                                        <RowDefinition Height="Auto"/>
                                                    </Grid.RowDefinitions>
                                                    
                                                    <!-- Thresholds Header -->
                                                    <TextBlock Grid.Row="0" Grid.Column="1" Text="{t_prefix} 1" Foreground="#0284C7" FontWeight="Bold" HorizontalAlignment="Center"/>
                                                    <TextBlock Grid.Row="0" Grid.Column="2" Text="{t_prefix} 2" Foreground="#0284C7" FontWeight="Bold" HorizontalAlignment="Center"/>
                                                    <TextBlock Grid.Row="0" Grid.Column="3" Text="{t_prefix} 3" Foreground="#0284C7" FontWeight="Bold" HorizontalAlignment="Center"/>
                                                    <TextBlock Grid.Row="0" Grid.Column="4" Text="{t_prefix} 4" Foreground="#0284C7" FontWeight="Bold" HorizontalAlignment="Center"/>
                                                    <TextBlock Grid.Row="0" Grid.Column="5" Text="{t_prefix} 5" Foreground="#0284C7" FontWeight="Bold" HorizontalAlignment="Center"/>
                                                    
                                                    <!-- Thresholds Inputs -->
                                                    <TextBlock Grid.Row="1" Grid.Column="0" Text="Thresholds" Style="{{StaticResource LabelStyle}}"/>
                                                    <TextBox Grid.Row="1" Grid.Column="1" Text="0"/>
                                                    <TextBox Grid.Row="1" Grid.Column="2" Text="0"/>
                                                    <TextBox Grid.Row="1" Grid.Column="3" Text="0"/>
                                                    <TextBox Grid.Row="1" Grid.Column="4" Text="0"/>
                                                    <TextBox Grid.Row="1" Grid.Column="5" Text="0"/>

                                                    <!-- Currents Header -->
                                                    <TextBlock Grid.Row="2" Grid.Column="1" Text="{c_prefix} 1-2" Foreground="#7C3AED" FontWeight="Bold" HorizontalAlignment="Center" Margin="0,15,0,0"/>
                                                    <TextBlock Grid.Row="2" Grid.Column="2" Text="{c_prefix} 2-3" Foreground="#7C3AED" FontWeight="Bold" HorizontalAlignment="Center" Margin="0,15,0,0"/>
                                                    <TextBlock Grid.Row="2" Grid.Column="3" Text="{c_prefix} 3-4" Foreground="#7C3AED" FontWeight="Bold" HorizontalAlignment="Center" Margin="0,15,0,0"/>
                                                    <TextBlock Grid.Row="2" Grid.Column="4" Text="{c_prefix} 4-5" Foreground="#7C3AED" FontWeight="Bold" HorizontalAlignment="Center" Margin="0,15,0,0"/>
                                                    
                                                    <!-- Currents Inputs -->
                                                    <TextBlock Grid.Row="3" Grid.Column="0" Text="Current Limits" Style="{{StaticResource LabelStyle}}"/>
                                                    <TextBox Grid.Row="3" Grid.Column="1" Text="0"/>
                                                    <TextBox Grid.Row="3" Grid.Column="2" Text="0"/>
                                                    <TextBox Grid.Row="3" Grid.Column="3" Text="0"/>
                                                    <TextBox Grid.Row="3" Grid.Column="4" Text="0"/>
                                                </Grid>
                                            </StackPanel>
                                        </GroupBox>
"""

stages = (
    get_stage_xaml("Cell Voltage Stages", "V", "C", "Cell V", "Current") +
    get_stage_xaml("Temperature Stages", "C", "C", "Temp", "Current") +
    get_stage_xaml("SOC Stages", "%", "C", "SOC", "Current")
)

final_xaml = create_xaml().replace("[STAGES_PLACEHOLDER]", stages)

with open("MainWindow.xaml", "w", encoding="utf-8") as f:
    f.write(final_xaml)
