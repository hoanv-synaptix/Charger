def get_stage_xaml(title, enabled_lbl, delta_lbl, unit_t, unit_c):
    return f"""
                                <GroupBox Header="{title}">
                                    <StackPanel>
                                        <StackPanel Orientation="Horizontal" Margin="0,0,0,5">
                                            <CheckBox Content="Enabled" Margin="0,0,20,0"/>
                                            <TextBlock Text="Delta ({unit_t}):" Width="60"/>
                                            <TextBox Text="0" Width="60"/>
                                        </StackPanel>
                                        <TextBlock Text="Thresholds ({unit_t})" Foreground="#355C7D" Margin="0,5,0,0"/>
                                        <UniformGrid Columns="5" Margin="0,0,0,5">
                                            <TextBox Text="0"/> <TextBox Text="0"/> <TextBox Text="0"/> <TextBox Text="0"/> <TextBox Text="0"/>
                                        </UniformGrid>
                                        <TextBlock Text="Current Limits ({unit_c})" Foreground="#6C5B7B" Margin="0,5,0,0"/>
                                        <UniformGrid Columns="4">
                                            <TextBox Text="0"/> <TextBox Text="0"/> <TextBox Text="0"/> <TextBox Text="0"/>
                                        </UniformGrid>
                                    </StackPanel>
                                </GroupBox>
    """

xaml = f"""<Window x:Class="ChargerDebugApp.MainWindow"
        xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
        xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
        Title="PKG Battery - Charger Debug App - UI v3 BMS (WPF Port)" Height="900" Width="1400"
        Background="#F0F0F0" WindowStartupLocation="CenterScreen">
    
    <Window.Resources>
        <Style TargetType="GroupBox">
            <Setter Property="Margin" Value="5"/>
            <Setter Property="Padding" Value="5"/>
            <Setter Property="Background" Value="#F0F0F0"/>
            <Setter Property="BorderBrush" Value="#CCCCCC"/>
            <Setter Property="FontWeight" Value="Bold"/>
        </Style>
        <Style TargetType="Button">
            <Setter Property="Margin" Value="2"/>
            <Setter Property="Padding" Value="10,5"/>
        </Style>
        <Style TargetType="TextBox">
            <Setter Property="Margin" Value="2"/>
            <Setter Property="VerticalContentAlignment" Value="Center"/>
        </Style>
        <Style TargetType="TextBlock">
            <Setter Property="VerticalAlignment" Value="Center"/>
            <Setter Property="Margin" Value="2"/>
        </Style>
        <Style TargetType="ComboBox">
            <Setter Property="Margin" Value="2"/>
        </Style>
    </Window.Resources>

    <Grid>
        <Grid.RowDefinitions>
            <RowDefinition Height="Auto"/> <!-- Connection Bar -->
            <RowDefinition Height="*"/>    <!-- Tabs -->
            <RowDefinition Height="Auto"/> <!-- Status Bar -->
        </Grid.RowDefinitions>

        <!-- CONNECTION BAR -->
        <Border Grid.Row="0" Background="#F0F0F0" BorderBrush="#CCCCCC" BorderThickness="0,0,0,1" Padding="5">
            <StackPanel Orientation="Horizontal" VerticalAlignment="Center">
                <TextBlock Text="Port:" Margin="10,0,5,0"/>
                <ComboBox Width="100"/>
                <Button Content="Refresh"/>
                <Button Content="Connect" Width="80"/>
                
                <Rectangle Width="1" Fill="#CCCCCC" Margin="10,0"/>
                <TextBlock Text="Baudrate:" Foreground="Gray"/>
                <TextBlock Text="115200" Margin="5,0,10,0"/>
                
                <Rectangle Width="1" Fill="#CCCCCC" Margin="10,0"/>
                <TextBlock Text="Disconnected" Foreground="Red" FontWeight="Bold" Margin="5,0"/>
                
                <CheckBox Content="Serial Debug Log" Margin="20,0,0,0" VerticalAlignment="Center"/>
            </StackPanel>
        </Border>

        <!-- MAIN TABS -->
        <TabControl Grid.Row="1" Margin="5" Background="#F0F0F0" BorderBrush="#CCCCCC">
            
            <!-- CONTROL TAB -->
            <TabItem Header="Control">
                <Grid Margin="5">
                    <Grid.ColumnDefinitions>
                        <ColumnDefinition Width="300"/>
                        <ColumnDefinition Width="*"/>
                    </Grid.ColumnDefinitions>
                    
                    <StackPanel Grid.Column="0">
                        <GroupBox Header="Module Management">
                            <StackPanel>
                                <StackPanel Orientation="Horizontal">
                                    <TextBlock Text="Address:" Width="50"/>
                                    <TextBox Text="0x01" Width="80"/>
                                    <Button Content="Add"/>
                                </StackPanel>
                                <StackPanel Orientation="Horizontal" Margin="0,5">
                                    <TextBlock Text="Driver:" Width="50"/>
                                    <ComboBox Width="120" SelectedIndex="0">
                                        <ComboBoxItem>Maxwell</ComboBoxItem>
                                    </ComboBox>
                                </StackPanel>
                            </StackPanel>
                        </GroupBox>
                    </StackPanel>
                    
                    <Grid Grid.Column="1" Margin="5,0,0,0">
                        <TextBlock Text="Control Tab Details here" HorizontalAlignment="Center"/>
                    </Grid>
                </Grid>
            </TabItem>

            <!-- MONITOR TAB -->
            <TabItem Header="Monitor">
                <Grid Margin="5">
                    <TextBlock Text="Monitor Tab Details here" HorizontalAlignment="Center"/>
                </Grid>
            </TabItem>

            <!-- CHARGE CONFIG TAB -->
            <TabItem Header="Charge Config">
                <Grid Margin="5">
                    <Grid.RowDefinitions>
                        <RowDefinition Height="Auto"/>
                        <RowDefinition Height="*"/>
                    </Grid.RowDefinitions>
                    
                    <StackPanel Grid.Row="0" Orientation="Horizontal" Margin="0,0,0,10">
                        <TextBlock Text="Charge Cycle Configuration" Foreground="#1F3A5F" FontWeight="Bold" FontSize="16" Margin="0,0,20,0"/>
                        <Button Content="Import"/>
                        <Button Content="Export"/>
                        <Button Content="Read MCU"/>
                        <Button Content="Write MCU"/>
                        <Button Content="Defaults"/>
                    </StackPanel>

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
                                                <TextBlock Text="Battery Cap (Ah):"/> <TextBox Text="0"/>
                                                <TextBlock Text="Temp Limit (C):"/> <TextBox Text="0"/>
                                                <TextBlock Text="V Min (V):"/> <TextBox Text="0"/>
                                                <TextBlock Text="V Max (V):"/> <TextBox Text="0"/>
                                                <TextBlock Text="I Min (C):"/> <TextBox Text="0"/>
                                                <TextBlock Text="I Max (C):"/> <TextBox Text="0"/>
                                            </UniformGrid>
                                        </GroupBox>
                                        <GroupBox Header="Charge Window">
                                            <UniformGrid Columns="4">
                                                <TextBlock Text="V Precharge (V):"/> <TextBox Text="0"/>
                                                <TextBlock Text="V Low (V):"/> <TextBox Text="0"/>
                                                <TextBlock Text="I Precharge (C):"/> <TextBox Text="0"/>
                                                <TextBlock Text="I Low (C):"/> <TextBox Text="0"/>
                                            </UniformGrid>
                                        </GroupBox>
                                    </StackPanel>
                                </GroupBox>

                                <GroupBox Header="Charging Strategy">
                                    <StackPanel>
                                        {get_stage_xaml("Cell Voltage Stages", "Enabled", "Delta", "V", "C")}
                                        {get_stage_xaml("Temperature Stages", "Enabled", "Delta", "C", "C")}
                                        {get_stage_xaml("SOC Stages", "Enabled", "Delta", "%", "C")}
                                    </StackPanel>
                                </GroupBox>
                            </StackPanel>

                            <StackPanel Grid.Column="1">
                                <GroupBox Header="Protection and Hardware">
                                    <StackPanel>
                                        <GroupBox Header="System Mapping">
                                            <UniformGrid Columns="2">
                                                <TextBlock Text="BMS CAN ID:"/> <TextBox Text="0x1801F456"/>
                                                <TextBlock Text="Charge Source:"/>
                                                <ComboBox SelectedIndex="0">
                                                    <ComboBoxItem>CAN Override</ComboBoxItem>
                                                    <ComboBoxItem>Manual Override</ComboBoxItem>
                                                    <ComboBoxItem>Auto (BMS)</ComboBoxItem>
                                                </ComboBox>
                                            </UniformGrid>
                                        </GroupBox>
                                        
                                        <GroupBox Header="Charge Jack Protection">
                                            <UniformGrid Columns="2">
                                                <CheckBox Content="Enabled" VerticalAlignment="Center"/> <TextBlock Text=""/>
                                                <TextBlock Text="Delta V (V):"/> <TextBox Text="0"/>
                                                <TextBlock Text="Delay (s):"/> <TextBox Text="0"/>
                                            </UniformGrid>
                                        </GroupBox>

                                        <GroupBox Header="Jack Temperature Protection">
                                            <UniformGrid Columns="2">
                                                <CheckBox Content="Enabled" VerticalAlignment="Center"/> <TextBlock Text=""/>
                                                <TextBlock Text="Threshold (C):"/> <TextBox Text="0"/>
                                                <TextBlock Text="Delta (C):"/> <TextBox Text="0"/>
                                                <TextBlock Text="Delay (s):"/> <TextBox Text="0"/>
                                                <TextBlock Text="Power Limit (%):"/> <TextBox Text="0"/>
                                            </UniformGrid>
                                        </GroupBox>

                                        <GroupBox Header="Charger Module Envelope">
                                            <UniformGrid Columns="2">
                                                <TextBlock Text="Module Type:"/>
                                                <ComboBox SelectedIndex="0">
                                                    <ComboBoxItem>Type A</ComboBoxItem>
                                                    <ComboBoxItem>Type B</ComboBoxItem>
                                                </ComboBox>
                                                <TextBlock Text="Source Modules:"/> <TextBox Text="2"/>
                                                <TextBlock Text="U Min (V):"/> <TextBox Text="0"/>
                                                <TextBlock Text="U Max (V):"/> <TextBox Text="0"/>
                                                <TextBlock Text="I Min (A):"/> <TextBox Text="0"/>
                                                <TextBlock Text="I Max (A):"/> <TextBox Text="0"/>
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
        <Border Grid.Row="2" Background="#007ACC" Padding="5">
            <StackPanel Orientation="Horizontal">
                <TextBlock Text="Ready" Foreground="White"/>
            </StackPanel>
        </Border>
    </Grid>
</Window>"""

with open("MainWindow.xaml", "w", encoding="utf-8") as f:
    f.write(xaml)
